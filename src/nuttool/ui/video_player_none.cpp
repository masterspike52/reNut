// Without Media Foundation (Linux): movies show their info but don't play.
#include "video_player.hpp"

namespace nut::gui {

struct VideoPlayer::Impl {};

VideoPlayer::VideoPlayer() = default;
VideoPlayer::~VideoPlayer() { close(); }

void VideoPlayer::closeSource() { impl_.reset(); }

bool VideoPlayer::openSource() {
    error_ = "movies play only in the Windows build";
    return false;
}

void VideoPlayer::seekSource(double) {}

void VideoPlayer::worker() {}

} // namespace nut::gui
