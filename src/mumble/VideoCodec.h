// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOCODEC_H_
#define MUMBLE_MUMBLE_VIDEOCODEC_H_

/// Supported video codecs for screen sharing.
/// Values must match MumbleUDP::Video::Codec enum in MumbleUDP.proto.
enum class VideoCodec {
	H264  = 0,  // H.264/AVC (widest compatibility)
	HEVC  = 1,  // H.265/HEVC (better compression)
	VP8   = 2,  // VP8 (WebRTC compatible)
	VP9   = 3,  // VP9 (better compression than VP8)
	AV1   = 4,  // AV1 (best compression, newer hardware required)
};

#endif // MUMBLE_MUMBLE_VIDEOCODEC_H_