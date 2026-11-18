#pragma once

// [The following text was written autonomously by an AI agent (Claude Code).]

class QWidget;

namespace mixxx {

/// Developer benchmark for scrolling the library track table, run by
/// tools/library_scroll_benchmark.py. Does nothing unless the environment
/// variable MIXXX_LIBRARY_SCROLL_BENCHMARK is set to a list of scenarios
/// `name:steps:rows:interval_ms,...` or to `default`.
///
/// Once the track table shows rows, it requests the images of all rows to
/// find the ones that don't exist, then clears QPixmapCache. Each scenario scrolls it `steps` times
/// by `rows` rows every `interval_ms`. Just before each step it counts the
/// visible cover and overview cells whose image is not in the cache yet,
/// i.e. that show a placeholder. After the last step it measures the time
/// until no placeholders remain. One result line per scenario is appended
/// to MIXXX_LIBRARY_SCROLL_BENCHMARK_OUTPUT (default: stdout), then Mixxx
/// exits.
void startLibraryScrollBenchmarkIfRequested(QWidget* pMainWindow);

} // namespace mixxx

// [End of the text written autonomously by an AI agent (Claude Code).]
