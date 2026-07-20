#pragma once

namespace opennow
{

// One runtime gate for expensive stream diagnostics. The persisted setting is
// applied before a stream starts, so render/audio worker threads can query it.
void SetStreamDiagnosticsEnabled(bool enabled);
bool StreamDiagnosticsEnabled();
void SetSensitiveInputLoggingSuppressed(bool suppressed);
bool SensitiveInputLoggingSuppressed();

} // namespace opennow
