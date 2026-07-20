// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENCAPTURE_H_
#define MUMBLE_MUMBLE_SCREENCAPTURE_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <cstdint>

#include "VideoCodec.h"

#ifdef USE_SCREEN_SHARING
#	include "CaptureSource.h"
extern "C" {
#	include <libavcodec/avcodec.h>
#	include <libavutil/opt.h>
#	include <libswscale/swscale.h>
}
#endif

/// Hardware encoder preference.
enum class HardwareEncoder {
	Auto,           // Automatically select best available hardware encoder
	Software,       // Force software encoding
	VideoToolbox,   // macOS VideoToolbox (H.264/HEVC)
	NVENC,          // NVIDIA NVENC (H.264/HEVC)
	VAAPI,          // Intel/AMD VAAPI (H.264/HEVC)
	QSV             // Intel Quick Sync Video (H.264/HEVC/VP9/AV1)
};

/// Configuration for screen capture encoding and performance.
struct ScreenCaptureConfig {
	int frameRate = 30;              // Target frame rate (1-60)
	int bitrate = 2500000;           // Target bitrate in bps (100kbps - 20Mbps)
	int keyframeInterval = 2;        // Keyframe interval in seconds (1-10)
	VideoCodec codec = VideoCodec::H264; // Video codec to use
	HardwareEncoder encoder = HardwareEncoder::Auto; // Encoder selection
	bool enableHardwareAccel = true; // Enable hardware acceleration
	bool adaptiveFrameRate = true;   // Reduce frame rate when content is static
	int minFrameRate = 5;            // Minimum frame rate when adaptive (1-30)
};

/// Captures a selected screen or window and emits encoded video frames via frameEncoded().
///
/// On macOS 14+, startCaptureNative() shows the OS-native SCContentSharingPicker and streams
/// frames via SCStream; captureStarted() / captureAborted() signals report the async outcome.
/// On Linux under Wayland, startCaptureNative() uses the xdg-desktop-portal ScreenCast interface
/// and delivers frames via a PipeWire stream.
/// On other platforms (or macOS < 14, or X11), use setSource() + startCapture() with ScreenPickerDialog.
///
/// Requires the build option -Dscreen-sharing=ON (links libavcodec/libswscale).
class ScreenCapture : public QObject {
private:
	Q_OBJECT
	Q_DISABLE_COPY(ScreenCapture)

public:
	explicit ScreenCapture(QObject *parent = nullptr);
	~ScreenCapture() override;

	void startCapture();
	void stopCapture();
	bool isCapturing() const;

	/// Returns current capture configuration.
	ScreenCaptureConfig config() const;
	/// Updates capture configuration (applies on next startCapture() or resolution change).
	void setConfig(const ScreenCaptureConfig &config);

	/// Returns the current encoder width (0 if not initialized).
	int encoderWidth() const;
	/// Returns the current encoder height (0 if not initialized).
	int encoderHeight() const;

#ifdef USE_SCREEN_SHARING
	/// Sets the capture source for the non-native picker path. Call before startCapture().
	void setSource(const CaptureSource &source);

#	if defined(Q_OS_MAC) || defined(HAS_WAYLAND_PORTAL)
	/// Shows the platform-native picker and starts capturing asynchronously.
	/// On macOS 14+: uses SCContentSharingPicker / SCStream.
	/// On Linux (Wayland): uses xdg-desktop-portal ScreenCast + PipeWire.
	/// captureStarted() is emitted when frames begin; captureAborted() if cancelled/failed.
	void startCaptureNative();
#	endif
#endif

signals:
	/// Emitted for every successfully encoded frame.
	void frameEncoded(QByteArray encodedData, quint64 frameNumber, bool isKeyFrame, VideoCodec codec);

	/// Emitted when an encoding or capture error occurs that requires stopping.
	void encodeError(QString errorMessage);

#ifdef USE_SCREEN_SHARING
#	if defined(Q_OS_MAC) || defined(HAS_WAYLAND_PORTAL)
	/// Emitted on the main thread when the native stream starts delivering frames.
	void captureStarted();
	/// Emitted on the main thread when the native picker is cancelled or the stream fails.
	void captureAborted();
#	endif
#endif

private slots:
	void captureFrame();

private:
#ifdef USE_SCREEN_SHARING
	bool initEncoder(int width, int height);
	bool tryInitHardwareEncoder(int width, int height);
	bool tryInitSoftwareEncoder(int width, int height);
	const char *selectEncoderName() const;
	void destroyEncoder();
	void encodeImage(const QImage &srcImage); ///< Shared encode path used by both capture modes.
	void handleEncodeError(const QString &error);
	void handleCaptureError(const QString &error);
	void updateTimerInterval();
	void loadConfigFromSettings();
	void saveConfigToSettings();

	CaptureSource m_source; ///< Defaults to EntireScreen, screenIndex=0 (primary display).

	AVCodecContext *m_codecCtx = nullptr;
	AVFrame *m_frame           = nullptr;
	AVPacket *m_packet         = nullptr;
	SwsContext *m_swsCtx       = nullptr;
	int m_encoderWidth         = 0;
	int m_encoderHeight        = 0;

	ScreenCaptureConfig m_config;
	QImage m_lastFrame; // For adaptive frame rate comparison
	int m_consecutiveErrors = 0;
#endif

	QTimer *m_captureTimer = nullptr;
	quint64 m_frameNumber  = 0;
	bool m_capturing       = false;
};

#endif // MUMBLE_MUMBLE_SCREENCAPTURE_H_