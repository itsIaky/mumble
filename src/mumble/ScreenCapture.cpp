// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenCapture.h"

#include "Log.h"

#ifdef USE_SCREEN_SHARING
#	include "CaptureSourceLister.h"
#	include <QtCore/QPointer>
#	include <QtCore/QSettings>
#	include <QtGui/QImage>
#	ifdef Q_OS_MAC
#		include "SCKitCapture.h"
#	elif defined(HAS_WAYLAND_PORTAL)
#		include "XdgPortalCapture.h"
#	endif
#endif

#include "Global.h"

static constexpr int DEFAULT_CAPTURE_INTERVAL_MS = 33;      // ~30 fps
static constexpr int DEFAULT_VIDEO_BITRATE       = 2500000; // 2.5 Mbps
static constexpr int DEFAULT_VIDEO_FPS           = 30;
static constexpr int MAX_CONSECUTIVE_ERRORS      = 10;

/// Returns the FFmpeg encoder name for the given codec and hardware preference.
static const char *encoderNameForCodec(VideoCodec codec, HardwareEncoder hwEncoder) {
	switch (codec) {
		case VideoCodec::H264:
			switch (hwEncoder) {
				case HardwareEncoder::VideoToolbox: return "h264_videotoolbox";
				case HardwareEncoder::NVENC:        return "h264_nvenc";
				case HardwareEncoder::VAAPI:        return "h264_vaapi";
				case HardwareEncoder::QSV:          return "h264_qsv";
				case HardwareEncoder::Software:     return "libx264";
				case HardwareEncoder::Auto:
				default:
#ifdef Q_OS_MAC
					if (avcodec_find_encoder_by_name("h264_videotoolbox")) return "h264_videotoolbox";
#elif defined(Q_OS_LINUX)
					if (avcodec_find_encoder_by_name("h264_nvenc")) return "h264_nvenc";
					if (avcodec_find_encoder_by_name("h264_vaapi")) return "h264_vaapi";
					if (avcodec_find_encoder_by_name("h264_qsv")) return "h264_qsv";
#endif
					return "libx264";
			}
		case VideoCodec::HEVC:
			switch (hwEncoder) {
				case HardwareEncoder::VideoToolbox: return "hevc_videotoolbox";
				case HardwareEncoder::NVENC:        return "hevc_nvenc";
				case HardwareEncoder::VAAPI:        return "hevc_vaapi";
				case HardwareEncoder::QSV:          return "hevc_qsv";
				case HardwareEncoder::Software:     return "libx265";
				case HardwareEncoder::Auto:
				default:
#ifdef Q_OS_MAC
					if (avcodec_find_encoder_by_name("hevc_videotoolbox")) return "hevc_videotoolbox";
#elif defined(Q_OS_LINUX)
					if (avcodec_find_encoder_by_name("hevc_nvenc")) return "hevc_nvenc";
					if (avcodec_find_encoder_by_name("hevc_vaapi")) return "hevc_vaapi";
					if (avcodec_find_encoder_by_name("hevc_qsv")) return "hevc_qsv";
#endif
					if (avcodec_find_encoder_by_name("libx265")) return "libx265";
					return nullptr;
			}
		case VideoCodec::VP8:
			switch (hwEncoder) {
				case HardwareEncoder::QSV:      return "vp8_qsv";
				case HardwareEncoder::Software: return "libvpx";
				case HardwareEncoder::Auto:
				default:
#ifdef Q_OS_LINUX
					if (avcodec_find_encoder_by_name("vp8_qsv")) return "vp8_qsv";
#endif
					if (avcodec_find_encoder_by_name("libvpx")) return "libvpx";
					return nullptr;
			}
		case VideoCodec::VP9:
			switch (hwEncoder) {
				case HardwareEncoder::QSV:      return "vp9_qsv";
				case HardwareEncoder::VAAPI:    return "vp9_vaapi";
				case HardwareEncoder::Software: return "libvpx-vp9";
				case HardwareEncoder::Auto:
				default:
#ifdef Q_OS_LINUX
					if (avcodec_find_encoder_by_name("vp9_qsv")) return "vp9_qsv";
					if (avcodec_find_encoder_by_name("vp9_vaapi")) return "vp9_vaapi";
#endif
					if (avcodec_find_encoder_by_name("libvpx-vp9")) return "libvpx-vp9";
					return nullptr;
			}
		case VideoCodec::AV1:
			switch (hwEncoder) {
				case HardwareEncoder::QSV:      return "av1_qsv";
				case HardwareEncoder::VAAPI:    return "av1_vaapi";
				case HardwareEncoder::NVENC:    return "av1_nvenc";
				case HardwareEncoder::Software: return "libaom-av1";
				case HardwareEncoder::Auto:
				default:
#ifdef Q_OS_LINUX
					if (avcodec_find_encoder_by_name("av1_qsv")) return "av1_qsv";
					if (avcodec_find_encoder_by_name("av1_vaapi")) return "av1_vaapi";
					if (avcodec_find_encoder_by_name("av1_nvenc")) return "av1_nvenc";
#endif
					if (avcodec_find_encoder_by_name("libaom-av1")) return "libaom-av1";
					return nullptr;
			}
	}
	return nullptr;
}

/// Returns the default pixel format for the given codec.
static AVPixelFormat defaultPixFmtForCodec(VideoCodec codec) {
	switch (codec) {
		case VideoCodec::H264:
		case VideoCodec::HEVC:
			return AV_PIX_FMT_YUV420P;
		case VideoCodec::VP8:
		case VideoCodec::VP9:
			return AV_PIX_FMT_YUV420P;
		case VideoCodec::AV1:
			return AV_PIX_FMT_YUV420P10LE; // AV1 prefers 10-bit
	}
	return AV_PIX_FMT_YUV420P;
}

ScreenCapture::ScreenCapture(QObject *parent) : QObject(parent) {
	m_captureTimer = new QTimer(this);
	m_captureTimer->setInterval(DEFAULT_CAPTURE_INTERVAL_MS);
	connect(m_captureTimer, &QTimer::timeout, this, &ScreenCapture::captureFrame);

	loadConfigFromSettings();
	updateTimerInterval();
}

ScreenCapture::~ScreenCapture() {
	stopCapture();
	saveConfigToSettings();
}

int ScreenCapture::encoderWidth() const {
#ifdef USE_SCREEN_SHARING
	return m_encoderWidth;
#else
	return 0;
#endif
}

int ScreenCapture::encoderHeight() const {
#ifdef USE_SCREEN_SHARING
	return m_encoderHeight;
#else
	return 0;
#endif
}

void ScreenCapture::startCapture() {
#ifndef USE_SCREEN_SHARING
	Global::get().l->log(Log::Warning,
						 QObject::tr("Screen sharing requires Mumble to be built with -Dscreen-sharing=ON."));
#else
	if (m_capturing)
		return;

	m_frameNumber = 0;
	m_capturing   = true;
	m_consecutiveErrors = 0;
	m_lastFrame = QImage();
	m_captureTimer->start();
#endif
}

void ScreenCapture::stopCapture() {
	if (!m_capturing)
		return;

	m_captureTimer->stop();
	m_capturing = false;

#ifdef USE_SCREEN_SHARING
#	ifdef Q_OS_MAC
	sckit_stop();
#	elif defined(HAS_WAYLAND_PORTAL)
	xdg_portal_stop();
#	endif
	destroyEncoder();
#endif
}

bool ScreenCapture::isCapturing() const {
	return m_capturing;
}

void ScreenCapture::setConfig(const ScreenCaptureConfig &config) {
	m_config = config;
	updateTimerInterval();
	saveConfigToSettings();
}

ScreenCaptureConfig ScreenCapture::config() const {
	return m_config;
}

#ifdef USE_SCREEN_SHARING

void ScreenCapture::setSource(const CaptureSource &source) {
	m_source = source;
	destroyEncoder(); // Reset so the encoder reinitialises at the new source's resolution.
}

#	if defined(Q_OS_MAC) || defined(HAS_WAYLAND_PORTAL)
void ScreenCapture::startCaptureNative() {
	if (m_capturing)
		return;

	// Keep a safe pointer — the lambdas below must not capture `this` without guard.
	QPointer< ScreenCapture > self = this;

	auto onStarted = [self]() {
		if (!self)
			return;
		self->m_capturing   = true;
		self->m_frameNumber = 0;
		self->m_consecutiveErrors = 0;
		self->m_lastFrame = QImage();
		emit self->captureStarted();
	};
	auto onCancelled = [self]() {
		if (!self)
			return;
		emit self->captureAborted();
	};
	auto onError = [self](QString error) {
		if (!self)
			return;
		Global::get().l->log(Log::Warning, QObject::tr("Screen capture failed: %1").arg(error));
		self->m_capturing = false;
		self->destroyEncoder();
		emit self->captureAborted();
	};
	auto onFrame = [self](QImage frame) {
		if (!self || !self->m_capturing)
			return;
		self->encodeImage(frame);
	};

#		ifdef Q_OS_MAC
	sckit_startWithNativePicker(std::move(onStarted), std::move(onCancelled), std::move(onError), std::move(onFrame));
#		else
	xdg_portal_startCapture(std::move(onStarted), std::move(onCancelled), std::move(onError), std::move(onFrame));
#		endif
}
#	endif // Q_OS_MAC || HAS_WAYLAND_PORTAL

void ScreenCapture::encodeImage(const QImage &srcImage) {
	// Caller must supply a non-null Format_RGBA8888 image.
	if (srcImage.isNull())
		return;

	// Adaptive frame rate: skip encoding if frame hasn't changed significantly
	if (m_config.adaptiveFrameRate && !m_lastFrame.isNull() && srcImage.size() == m_lastFrame.size()) {
		// Quick check: compare a few sample pixels
		const int stride = qMax(1, srcImage.width() / 16);
		bool changed = false;
		for (int y = 0; y < srcImage.height() && !changed; y += stride) {
			for (int x = 0; x < srcImage.width() && !changed; x += stride) {
				if (srcImage.pixel(x, y) != m_lastFrame.pixel(x, y)) {
					changed = true;
				}
			}
		}
		if (!changed) {
			// Content hasn't changed, skip this frame but ensure minimum frame rate
			if (m_captureTimer->interval() < 1000 / m_config.minFrameRate) {
				m_captureTimer->setInterval(1000 / m_config.minFrameRate);
			}
			return;
		}
	}
	m_lastFrame = srcImage.copy();

	// Convert to Format_RGBA8888 for mapping to AV_PIX_FMT_RGBA
	QImage image = srcImage.convertToFormat(QImage::Format_RGBA8888);
	// libx264 (YUV420P) requires even dimensions — crop one pixel if needed.
	const int width  = image.width() & ~1;
	const int height = image.height() & ~1;
	if (width <= 0 || height <= 0)
		return;
	if (width != image.width() || height != image.height())
		image = image.copy(0, 0, width, height);

	// (Re-)initialise the encoder when the resolution changes.
	if (!m_codecCtx || m_encoderWidth != width || m_encoderHeight != height) {
		destroyEncoder();
		if (!initEncoder(width, height))
			return;
	}

	// Colour-space conversion: RGBA to YUV420P (or codec-specific format).
	AVPixelFormat targetPixFmt = defaultPixFmtForCodec(m_config.codec);
	m_swsCtx = sws_getCachedContext(m_swsCtx, width, height, AV_PIX_FMT_RGBA, width, height, targetPixFmt,
									SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (!m_swsCtx)
		return;

	if (av_frame_make_writable(m_frame) < 0)
		return;

	const uint8_t *srcData[1] = { image.constBits() };
	int srcLinesize[1]        = { static_cast< int >(image.bytesPerLine()) };
	sws_scale(m_swsCtx, srcData, srcLinesize, 0, height, m_frame->data, m_frame->linesize);

	m_frame->pts = static_cast< int64_t >(m_frameNumber);

	if (avcodec_send_frame(m_codecCtx, m_frame) < 0) {
		handleEncodeError(tr("Failed to send frame to encoder"));
		return;
	}

	while (avcodec_receive_packet(m_codecCtx, m_packet) == 0) {
		QByteArray encodedData(reinterpret_cast< const char * >(m_packet->data), m_packet->size);
		const bool isKey = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
		emit frameEncoded(encodedData, m_frameNumber, isKey, m_config.codec);
		av_packet_unref(m_packet);
	}

	++m_frameNumber;
	m_consecutiveErrors = 0; // Reset error counter on success

	// Restore normal frame rate after successful encode
	if (m_config.adaptiveFrameRate && m_captureTimer->interval() > 1000 / m_config.frameRate) {
		m_captureTimer->setInterval(1000 / m_config.frameRate);
	}
}

#	endif // USE_SCREEN_SHARING

void ScreenCapture::captureFrame() {
#ifdef USE_SCREEN_SHARING
	// Delegate platform-specific grab to CaptureSourceLister.
	QImage image = grabCaptureSource(m_source);
	if (image.isNull()) {
		handleCaptureError(tr("Screen capture returned null image"));
		return;
	}

	// Ensure Format_RGBA8888 for AV_PIX_FMT_RGBA mapping.
	encodeImage(image.convertToFormat(QImage::Format_RGBA8888));
#endif
}

#ifdef USE_SCREEN_SHARING

bool ScreenCapture::initEncoder(int width, int height) {
	// Try hardware encoder first if enabled
	if (m_config.enableHardwareAccel && m_config.encoder != HardwareEncoder::Software) {
		if (tryInitHardwareEncoder(width, height)) {
			return true;
		}
		Global::get().l->log(Log::Information, tr("Hardware encoder unavailable, falling back to software encoding"));
	}

	// Fall back to software encoder
	return tryInitSoftwareEncoder(width, height);
}

bool ScreenCapture::tryInitHardwareEncoder(int width, int height) {
	const char *encoderName = encoderNameForCodec(m_config.codec, m_config.encoder);
	if (!encoderName) {
		Global::get().l->log(Log::Warning, tr("No hardware encoder available for codec %1").arg(static_cast<int>(m_config.codec)));
		return false;
	}

	const AVCodec *codec = avcodec_find_encoder_by_name(encoderName);
	if (!codec) {
		Global::get().l->log(Log::Warning, tr("Hardware encoder '%1' not available").arg(QString::fromLatin1(encoderName)));
		return false;
	}

	m_codecCtx = avcodec_alloc_context3(codec);
	if (!m_codecCtx)
		return false;

	m_codecCtx->width     = width;
	m_codecCtx->height    = height;
	m_codecCtx->time_base = { 1, m_config.frameRate };
	m_codecCtx->pix_fmt   = defaultPixFmtForCodec(m_config.codec);
	m_codecCtx->bit_rate  = m_config.bitrate;
	m_codecCtx->gop_size  = m_config.frameRate * m_config.keyframeInterval;
	m_codecCtx->max_b_frames = 0; // No B-frames for low latency

	// Encoder-specific options
	if (strcmp(encoderName, "h264_videotoolbox") == 0 || strcmp(encoderName, "hevc_videotoolbox") == 0) {
		// VideoToolbox options
		av_opt_set(m_codecCtx->priv_data, "realtime", "1", 0);
		av_opt_set(m_codecCtx->priv_data, "allow_sw", "0", 0); // Fail if hardware not available
	} else if (strstr(encoderName, "nvenc") != nullptr) {
		// NVENC options
		av_opt_set(m_codecCtx->priv_data, "preset", "p1", 0); // Fastest preset
		av_opt_set(m_codecCtx->priv_data, "tune", "ll", 0);   // Low latency
		av_opt_set(m_codecCtx->priv_data, "rc", "cbr", 0);    // Constant bitrate
	} else if (strstr(encoderName, "vaapi") != nullptr) {
		// VAAPI options
		av_opt_set(m_codecCtx->priv_data, "preset", "fast", 0);
		av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency", 0);
	} else if (strstr(encoderName, "qsv") != nullptr) {
		// QSV options
		av_opt_set(m_codecCtx->priv_data, "preset", "veryfast", 0);
		av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency", 0);
	} else if (strstr(encoderName, "libvpx") != nullptr || strstr(encoderName, "libaom") != nullptr) {
		// VP8/VP9/AV1 software encoder options
		av_opt_set(m_codecCtx->priv_data, "deadline", "realtime", 0);
		av_opt_set(m_codecCtx->priv_data, "cpu-used", "8", 0); // Fastest
		av_opt_set(m_codecCtx->priv_data, "lag-in-frames", "0", 0);
		av_opt_set(m_codecCtx->priv_data, "error-resilient", "1", 0);
	}

	if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
		avcodec_free_context(&m_codecCtx);
		return false;
	}

	m_frame = av_frame_alloc();
	m_frame->format = defaultPixFmtForCodec(m_config.codec);
	m_frame->width  = width;
	m_frame->height = height;
	if (av_frame_get_buffer(m_frame, 0) < 0) {
		av_frame_free(&m_frame);
		avcodec_free_context(&m_codecCtx);
		return false;
	}

	m_packet = av_packet_alloc();

	m_encoderWidth  = width;
	m_encoderHeight = height;

	Global::get().l->log(Log::Information, tr("Initialized hardware encoder: %1").arg(QString::fromLatin1(encoderName)));
	return true;
}

bool ScreenCapture::tryInitSoftwareEncoder(int width, int height) {
	const char *encoderName = encoderNameForCodec(m_config.codec, HardwareEncoder::Software);
	if (!encoderName) {
		Global::get().l->log(Log::Warning, tr("No software encoder available for codec %1").arg(static_cast<int>(m_config.codec)));
		return false;
	}

	const AVCodec *codec = avcodec_find_encoder_by_name(encoderName);
	if (!codec) {
Global::get().l->log(Log::Warning,
						 tr("Encoder '%1' not available. Ensure it is installed and libavcodec was compiled with it.")
						 .arg(QString::fromLatin1(encoderName)));
		return false;
	}

	m_codecCtx = avcodec_alloc_context3(codec);
	if (!m_codecCtx)
		return false;

	m_codecCtx->width     = width;
	m_codecCtx->height    = height;
	m_codecCtx->time_base = { 1, m_config.frameRate };
	m_codecCtx->pix_fmt   = defaultPixFmtForCodec(m_config.codec);
	m_codecCtx->bit_rate  = m_config.bitrate;
	m_codecCtx->gop_size  = m_config.frameRate * m_config.keyframeInterval;
	m_codecCtx->max_b_frames = 0;

	// Minimise encoding latency
	if (strstr(encoderName, "libx264") || strstr(encoderName, "libx265")) {
		av_opt_set(m_codecCtx->priv_data, "preset", "superfast", 0);
		av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency", 0);
		av_opt_set(m_codecCtx->priv_data, "profile", "baseline", 0); // Maximum compatibility
	} else if (strstr(encoderName, "libvpx") || strstr(encoderName, "libaom")) {
		av_opt_set(m_codecCtx->priv_data, "deadline", "realtime", 0);
		av_opt_set(m_codecCtx->priv_data, "cpu-used", "8", 0);
		av_opt_set(m_codecCtx->priv_data, "lag-in-frames", "0", 0);
		av_opt_set(m_codecCtx->priv_data, "error-resilient", "1", 0);
	}

	if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
		avcodec_free_context(&m_codecCtx);
		return false;
	}

	m_frame = av_frame_alloc();
	m_frame->format = defaultPixFmtForCodec(m_config.codec);
	m_frame->width  = width;
	m_frame->height = height;
	if (av_frame_get_buffer(m_frame, 0) < 0) {
		av_frame_free(&m_frame);
		avcodec_free_context(&m_codecCtx);
		return false;
	}

	m_packet = av_packet_alloc();

	m_encoderWidth  = width;
	m_encoderHeight = height;

	Global::get().l->log(Log::Information, tr("Initialized software encoder: %1").arg(QString::fromLatin1(encoderName)));
	return true;
}

void ScreenCapture::destroyEncoder() {
	if (m_swsCtx) {
		sws_freeContext(m_swsCtx);
		m_swsCtx = nullptr;
	}
	if (m_frame) {
		av_frame_free(&m_frame);
	}
	if (m_packet) {
		av_packet_free(&m_packet);
	}
	if (m_codecCtx) {
		avcodec_free_context(&m_codecCtx);
	}
	m_encoderWidth  = 0;
	m_encoderHeight = 0;
}

void ScreenCapture::handleEncodeError(const QString &error) {
	m_consecutiveErrors++;
	Global::get().l->log(Log::Warning, tr("Encode error (%1/%2): %3")
						 .arg(m_consecutiveErrors).arg(MAX_CONSECUTIVE_ERRORS).arg(error));

	if (m_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
		emit encodeError(tr("Too many consecutive encoding errors, stopping capture: %1").arg(error));
		stopCapture();
	}
}

void ScreenCapture::handleCaptureError(const QString &error) {
	m_consecutiveErrors++;
	Global::get().l->log(Log::Warning, tr("Capture error (%1/%2): %3")
						 .arg(m_consecutiveErrors).arg(MAX_CONSECUTIVE_ERRORS).arg(error));

	if (m_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
		emit encodeError(tr("Too many consecutive capture errors, stopping capture: %1").arg(error));
		stopCapture();
	}
}

void ScreenCapture::updateTimerInterval() {
	if (m_captureTimer) {
		m_captureTimer->setInterval(1000 / qBound(1, m_config.frameRate, 60));
	}
}

void ScreenCapture::loadConfigFromSettings() {
	QSettings settings;
	settings.beginGroup("ScreenCapture");
	m_config.frameRate = settings.value("frameRate", DEFAULT_VIDEO_FPS).toInt();
	m_config.bitrate = settings.value("bitrate", DEFAULT_VIDEO_BITRATE).toInt();
	m_config.keyframeInterval = settings.value("keyframeInterval", 2).toInt();
	m_config.codec = static_cast<VideoCodec>(settings.value("codec", static_cast<int>(VideoCodec::H264)).toInt());
	m_config.encoder = static_cast<HardwareEncoder>(
		settings.value("encoder", static_cast<int>(HardwareEncoder::Auto)).toInt());
	m_config.enableHardwareAccel = settings.value("enableHardwareAccel", true).toBool();
	m_config.adaptiveFrameRate = settings.value("adaptiveFrameRate", true).toBool();
	m_config.minFrameRate = settings.value("minFrameRate", 5).toInt();
	settings.endGroup();

	// Clamp values to valid ranges
	m_config.frameRate = qBound(1, m_config.frameRate, 60);
	m_config.bitrate = qBound(100000, m_config.bitrate, 20000000);
	m_config.keyframeInterval = qBound(1, m_config.keyframeInterval, 10);
	m_config.minFrameRate = qBound(1, m_config.minFrameRate, m_config.frameRate);
}

void ScreenCapture::saveConfigToSettings() {
	QSettings settings;
	settings.beginGroup("ScreenCapture");
	settings.setValue("frameRate", m_config.frameRate);
	settings.setValue("bitrate", m_config.bitrate);
	settings.setValue("keyframeInterval", m_config.keyframeInterval);
	settings.setValue("codec", static_cast<int>(m_config.codec));
	settings.setValue("encoder", static_cast<int>(m_config.encoder));
	settings.setValue("enableHardwareAccel", m_config.enableHardwareAccel);
	settings.setValue("adaptiveFrameRate", m_config.adaptiveFrameRate);
	settings.setValue("minFrameRate", m_config.minFrameRate);
	settings.endGroup();
}

#endif // USE_SCREEN_SHARING