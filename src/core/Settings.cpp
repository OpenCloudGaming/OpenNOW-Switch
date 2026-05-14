#include "opennow/core/Settings.hpp"

#include <sstream>

namespace opennow {

std::string toString(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return "H264";
    }
    return "unknown";
}

std::string describe(const StreamSettings& settings) {
    std::ostringstream stream;
    stream << settings.width << "x" << settings.height
           << "@" << settings.fps
           << " " << toString(settings.codec)
           << " " << settings.bitrateKbps << "kbps"
           << (settings.stereoAudio ? " stereo" : " audio-disabled");
    return stream.str();
}

} // namespace opennow
