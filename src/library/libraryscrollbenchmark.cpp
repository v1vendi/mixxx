// [The following text was written autonomously by an AI agent (Claude Code).]

#include "library/libraryscrollbenchmark.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QPixmapCache>
#include <QScrollBar>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <cstdio>

#include "control/controlobject.h"
#include "library/coverartcache.h"
#include "library/overviewcache.h"
#include "library/tabledelegates/coverartdelegate.h"
#include "library/tabledelegates/overviewdelegate.h"
#include "library/trackmodel.h"
#include "waveform/overviewtype.h"
#include "waveform/renderers/waveformsignalcolors.h"
#include "widget/wtracktableview.h"

#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif
#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#endif

namespace {

const QString kDefaultScenarios = QStringLiteral(
        "browse:20:15:700,wheel:30:5:100,flick:40:5:25,back:15:-15:300");
constexpr int kSettleTimeoutMs = 10000;
// Wait before the first scenario, so the view repainted after the probe
constexpr int kStartDelayMs = 3000;
constexpr int kProbeTimeoutMs = 60000;
// Wait after a scenario settled, so requests of the window finish
// before CPU and memory are read
constexpr int kTailMs = 3000;

struct Scenario {
    QString name;
    int steps = 0;
    int rows = 0;
    int intervalMs = 0;
};

QList<Scenario> parseScenarios(const QString& spec) {
    QList<Scenario> scenarios;
    const QStringList items = (spec == QStringLiteral("default") ? kDefaultScenarios : spec)
                                      .split(QChar(','), Qt::SkipEmptyParts);
    for (const QString& item : items) {
        const QStringList parts = item.split(QChar(':'));
        if (parts.size() != 4) {
            qWarning() << "Library scroll benchmark: invalid scenario" << item;
            return {};
        }
        scenarios.append(Scenario{parts[0],
                parts[1].toInt(),
                parts[2].toInt(),
                std::max(1, parts[3].toInt())});
    }
    return scenarios;
}

double cpuSeconds() {
#if defined(Q_OS_UNIX)
    rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
            usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
#else
    return -1;
#endif
}

/// Current memory in MB: the footprint shown by Activity Monitor on macOS,
/// the resident set size on Linux
double memoryMb() {
#if defined(Q_OS_MACOS)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(),
                TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info),
                &count) != KERN_SUCCESS) {
        return -1;
    }
    return info.phys_footprint / 1048576.0;
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/self/status"));
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    for (const QByteArray& line : file.readAll().split('\n')) {
        if (line.startsWith("VmRSS:")) {
            return line.mid(6).trimmed().split(' ').first().toDouble() / 1024;
        }
    }
    return -1;
#else
    return -1;
#endif
}

class LibraryScrollBenchmark : public QObject {
  public:
    LibraryScrollBenchmark(QWidget* pMainWindow,
            QList<Scenario> scenarios,
            QString outputPath)
            : QObject(pMainWindow),
              m_pMainWindow(pMainWindow),
              m_scenarios(std::move(scenarios)),
              m_outputPath(std::move(outputPath)) {
        // Tracks without an image never get one, don't count them
        connect(OverviewCache::instance(),
                &OverviewCache::overviewReady,
                this,
                [this](const QObject*, TrackId trackId, bool pixmapValid) {
                    m_probedOverviews.remove(trackId);
                    if (!pixmapValid) {
                        m_tracksWithoutOverview.insert(trackId);
                    }
                });
        connect(CoverArtCache::instance(),
                &CoverArtCache::coverFound,
                this,
                [this](const QObject*, const CoverInfo& coverInfo, const QPixmap& pixmap) {
                    m_probedCovers.remove(coverInfo.cacheKey());
                    if (pixmap.isNull()) {
                        m_missingCovers.insert(coverInfo.cacheKey());
                    }
                });
        m_pTimer = new QTimer(this);
        waitForTable();
    }

  private:
    struct Blank {
        int overviews = 0;
        int covers = 0;
        int cells = 0;
    };

    void waitForTable() {
        m_pTimer->disconnect();
        connect(m_pTimer, &QTimer::timeout, this, [this]() {
            m_pView = findTable();
            if (!m_pView) {
                return;
            }
            m_pTimer->stop();
            m_pView->window()->raise();
            m_pView->window()->activateWindow();
            m_pView->scrollToTop();
            probeImages();
        });
        m_pTimer->start(500);
    }

    /// Requests the images of all rows, to find the cells that never get
    /// one. Otherwise they would count as blank only in builds that don't
    /// request them. Then clears QPixmapCache, so all builds start cold.
    void probeImages() {
        auto* pTrackModel = dynamic_cast<TrackModel*>(m_pView->model());
        const QAbstractItemModel* pModel = m_pView->model();
        const double scaleFactor = m_pView->devicePixelRatioF();
        const auto overviewType = currentOverviewType();
        const WaveformSignalColors signalColors;
        const int firstRow = std::max(0, m_pView->rowAt(0));
        for (int row = 0; pTrackModel && row < pModel->rowCount(); ++row) {
            if (m_coverColumn >= 0) {
                const QModelIndex index = pModel->index(row, m_coverColumn);
                const CoverInfo coverInfo = pTrackModel->getCoverInfo(index);
                // Legacy covers need the track, they are counted as is
                if (coverInfo.hasImage() && !coverInfo.imageDigest().isEmpty()) {
                    m_probedCovers.insert(coverInfo.cacheKey());
                    CoverArtCache::requestUncachedCover(this,
                            coverInfo,
                            static_cast<int>(m_pView->visualRect(pModel->index(
                                                                         firstRow, m_coverColumn))
                                                     .width() *
                                    scaleFactor));
                }
            }
            if (m_overviewColumn >= 0) {
                const TrackId trackId = pTrackModel->getTrackId(
                        pModel->index(row, m_overviewColumn));
                if (trackId.isValid()) {
                    m_probedOverviews.insert(trackId);
                    const QPixmap pixmap = OverviewCache::instance()->requestUncachedOverview(
                            overviewType,
                            signalColors,
                            trackId,
                            this,
                            m_pView->visualRect(pModel->index(firstRow, m_overviewColumn))
                                            .size() *
                                    scaleFactor);
                    if (!pixmap.isNull()) {
                        m_probedOverviews.remove(trackId);
                    }
                }
            }
        }
        QElapsedTimer probeTimer;
        probeTimer.start();
        m_pTimer->disconnect();
        connect(m_pTimer, &QTimer::timeout, this, [this, probeTimer]() {
            if ((!m_probedCovers.isEmpty() || !m_probedOverviews.isEmpty()) &&
                    probeTimer.elapsed() < kProbeTimeoutMs) {
                return;
            }
            m_pTimer->stop();
            // No answer: OverviewCache already knew it has no overview
            m_tracksWithoutOverview.unite(m_probedOverviews);
            QPixmapCache::clear();
            m_pView->viewport()->update();
            QTimer::singleShot(kStartDelayMs, this, [this]() { startScenario(); });
        });
        m_pTimer->start(100);
    }

    mixxx::OverviewType currentOverviewType() const {
        return static_cast<mixxx::OverviewType>(static_cast<int>(
                ControlObject::get(ConfigKey(QStringLiteral("[Waveform]"),
                        QStringLiteral("WaveformOverviewType")))));
    }

    /// The largest visible track table with rows and a cover or overview column
    WTrackTableView* findTable() {
        WTrackTableView* pBest = nullptr;
        int bestArea = 0;
        const auto views = m_pMainWindow->findChildren<WTrackTableView*>();
        for (WTrackTableView* pView : views) {
            const int area = pView->width() * pView->height();
            if (!pView->isVisible() || !pView->model() ||
                    pView->model()->rowCount() == 0 || area <= bestArea) {
                continue;
            }
            m_coverColumn = -1;
            m_overviewColumn = -1;
            for (int column = 0; column < pView->model()->columnCount(); ++column) {
                if (pView->isColumnHidden(column)) {
                    continue;
                }
                QAbstractItemDelegate* pDelegate = pView->itemDelegateForColumn(column);
                if (qobject_cast<CoverArtDelegate*>(pDelegate)) {
                    m_coverColumn = column;
                } else if (qobject_cast<OverviewDelegate*>(pDelegate)) {
                    m_overviewColumn = column;
                }
            }
            if (m_coverColumn >= 0 || m_overviewColumn >= 0) {
                pBest = pView;
                bestArea = area;
            }
        }
        return pBest;
    }

    Blank countBlankCells() {
        Blank blank;
        auto* pTrackModel = dynamic_cast<TrackModel*>(m_pView->model());
        if (!pTrackModel) {
            return blank;
        }
        const QAbstractItemModel* pModel = m_pView->model();
        const double scaleFactor = m_pView->devicePixelRatioF();
        const auto overviewType = currentOverviewType();
        const int first = std::max(0, m_pView->rowAt(0));
        int last = m_pView->rowAt(m_pView->viewport()->height() - 1);
        if (last < 0) {
            last = pModel->rowCount() - 1;
        }
        for (int row = first; row <= last; ++row) {
            if (m_coverColumn >= 0) {
                const QModelIndex index = pModel->index(row, m_coverColumn);
                const CoverInfo coverInfo = pTrackModel->getCoverInfo(index);
                if (coverInfo.hasImage() && !m_missingCovers.contains(coverInfo.cacheKey())) {
                    ++blank.cells;
                    const int width = static_cast<int>(
                            m_pView->visualRect(index).width() * scaleFactor);
                    if (CoverArtCache::getCachedCover(coverInfo, width).isNull()) {
                        ++blank.covers;
                    }
                }
            }
            if (m_overviewColumn >= 0) {
                const QModelIndex index = pModel->index(row, m_overviewColumn);
                const TrackId trackId = pTrackModel->getTrackId(index);
                if (trackId.isValid() && !m_tracksWithoutOverview.contains(trackId)) {
                    ++blank.cells;
                    if (OverviewCache::instance()
                                    ->requestCachedOverview(overviewType,
                                            trackId,
                                            this,
                                            m_pView->visualRect(index).size() * scaleFactor)
                                    .isNull()) {
                        ++blank.overviews;
                    }
                }
            }
        }
        return blank;
    }

    void startScenario() {
        if (m_scenarios.isEmpty()) {
            QCoreApplication::exit(0);
            return;
        }
        m_scenario = m_scenarios.takeFirst();
        m_step = 0;
        m_blank = Blank{};
        m_stepsWithBlank = 0;
        m_cpuStart = cpuSeconds();
        m_maxMemoryMb = memoryMb();
        m_pTimer->disconnect();
        connect(m_pTimer, &QTimer::timeout, this, [this]() { step(); });
        m_pTimer->start(m_scenario.intervalMs);
    }

    void step() {
        m_maxMemoryMb = std::max(m_maxMemoryMb, memoryMb());
        if (m_step > 0) {
            const Blank blank = countBlankCells();
            m_blank.overviews += blank.overviews;
            m_blank.covers += blank.covers;
            m_blank.cells += blank.cells;
            if (blank.overviews + blank.covers > 0) {
                ++m_stepsWithBlank;
            }
        }
        if (m_step == m_scenario.steps) {
            settle();
            return;
        }
        QScrollBar* pScrollBar = m_pView->verticalScrollBar();
        const int rowStep = m_pView->verticalScrollMode() == QAbstractItemView::ScrollPerItem
                ? 1
                : m_pView->rowHeight(0);
        pScrollBar->setValue(pScrollBar->value() + m_scenario.rows * rowStep);
        ++m_step;
    }

    void settle() {
        QElapsedTimer settleTimer;
        settleTimer.start();
        m_pTimer->disconnect();
        connect(m_pTimer, &QTimer::timeout, this, [this, settleTimer]() {
            m_maxMemoryMb = std::max(m_maxMemoryMb, memoryMb());
            const Blank blank = countBlankCells();
            const qint64 settleMs = settleTimer.elapsed();
            if (blank.overviews + blank.covers > 0 && settleMs < kSettleTimeoutMs) {
                return;
            }
            m_pTimer->stop();
            QTimer::singleShot(kTailMs, this, [this, settleMs]() {
                writeResult(settleMs);
                startScenario();
            });
        });
        m_pTimer->start(10);
    }

    void writeResult(qint64 settleMs) {
        const QString line =
                QStringLiteral(
                        "scenario=%1 steps=%2 rows=%3 interval_ms=%4 "
                        "blank_overviews=%5 blank_covers=%6 sampled_cells=%7 "
                        "steps_with_blank=%8 settle_ms=%9 cpu_s=%10 "
                        "memory_mb=%11 max_memory_mb=%12")
                        .arg(m_scenario.name)
                        .arg(m_scenario.steps)
                        .arg(m_scenario.rows)
                        .arg(m_scenario.intervalMs)
                        .arg(m_blank.overviews)
                        .arg(m_blank.covers)
                        .arg(m_blank.cells)
                        .arg(m_stepsWithBlank)
                        .arg(settleMs >= kSettleTimeoutMs ? -1 : settleMs)
                        .arg(cpuSeconds() - m_cpuStart, 0, 'f', 2)
                        .arg(memoryMb(), 0, 'f', 1)
                        .arg(std::max(m_maxMemoryMb, memoryMb()), 0, 'f', 1);
        QFile file(m_outputPath);
        if (m_outputPath.isEmpty()
                        ? file.open(stdout, QIODevice::WriteOnly)
                        : file.open(QIODevice::Append | QIODevice::Text)) {
            file.write(line.toUtf8() + '\n');
        }
    }

    QWidget* const m_pMainWindow;
    QList<Scenario> m_scenarios;
    const QString m_outputPath;
    QTimer* m_pTimer = nullptr;
    WTrackTableView* m_pView = nullptr;
    int m_coverColumn = -1;
    int m_overviewColumn = -1;
    QSet<TrackId> m_tracksWithoutOverview;
    QSet<mixxx::cache_key_t> m_missingCovers;
    QSet<TrackId> m_probedOverviews;
    QSet<mixxx::cache_key_t> m_probedCovers;

    Scenario m_scenario;
    int m_step = 0;
    Blank m_blank;
    int m_stepsWithBlank = 0;
    double m_cpuStart = 0;
    double m_maxMemoryMb = 0;
};

} // namespace

namespace mixxx {

void startLibraryScrollBenchmarkIfRequested(QWidget* pMainWindow) {
    const QString spec = qEnvironmentVariable("MIXXX_LIBRARY_SCROLL_BENCHMARK");
    if (spec.isEmpty() || !OverviewCache::instance() || !CoverArtCache::instance()) {
        return;
    }
    QList<Scenario> scenarios = parseScenarios(spec);
    if (scenarios.isEmpty()) {
        return;
    }
    new LibraryScrollBenchmark(pMainWindow,
            std::move(scenarios),
            qEnvironmentVariable("MIXXX_LIBRARY_SCROLL_BENCHMARK_OUTPUT"));
}

} // namespace mixxx

// [End of the text written autonomously by an AI agent (Claude Code).]
