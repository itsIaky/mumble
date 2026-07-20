// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareReceiver.h"

#include "Global.h"
#include "Log.h"
#include "MumbleProtocol.h"

#ifdef USE_SCREEN_SHARING
/// Maps the protocol's Codec enum to the corresponding FFmpeg codec ID.
/// To add support for a new codec: add the proto enum value in MumbleUDP.proto,
/// then add a case here returning the appropriate AV_CODEC_ID_*.
static AVCodecID codecIdForProtoCodec(VideoCodec c) {
	switch (c) {
		case VideoCodec::H264:
			return AV_CODEC_ID_H264;
		case VideoCodec::HEVC:
			return AV_CODEC_ID_HEVC;
		case VideoCodec::VP8:
			return AV_CODEC_ID_VP8;
		case VideoCodec::VP9:
			return AV_CODEC_ID_VP9;
		case VideoCodec::AV1:
			return AV_CODEC_ID_AV1;
		default:
			return AV_CODEC_ID_NONE;
	}
}

/// Returns the codec name for logging.
static const char *codecName(VideoCodec c) {
	switch (c) {
		case VideoCodec::H264: return "H.264";
		case VideoCodec::HEVC: return "HEVC";
		case VideoCodec::VP8:  return "VP8";
		case VideoCodec::VP9:  return "VP9";
		case VideoCodec::AV1:  return "AV1";
		default: return "Unknown";
	}
}
#endif

ScreenShareReceiver::ScreenShareReceiver(QObject *parent) : QObject(parent) {
}

ScreenShareReceiver::~ScreenShareReceiver() {
#ifdef USE_SCREEN_SHARING
	// Tear down all decoders.
	for (std::pair< const unsigned int, DecoderState > &kv : m_decoders) {
		DecoderState &ds = kv.second;
		if (ds.swsCtx) {
			sws_freeContext(ds.swsCtx);
		}
		if (ds.frame) {
			av_frame_free(&ds.frame);
		}
		if (ds.packet) {
			av_packet_free(&ds.packet);
		}
		if (ds.codecCtx) {
			avcodec_free_context(&ds.codecCtx);
		}
	}
#endif
}


void ScreenShareReceiver::handleVideoPacket(const Mumble::Protocol::VideoData &videoData) {
#ifndef USE_SCREEN_SHARING
	Q_UNUSED(videoData);
#else
	const quint32 session   = videoData.senderSession;
	const quint64 frameNum  = videoData.frameNumber;
	const quint32 fragIdx   = videoData.fragmentIndex;
	const quint32 fragCount = videoData.fragmentCount;

	if (fragCount == 0 || fragIdx >= fragCount)
		return;

	PendingFrame &pf = m_fragmentBuffer[session][frameNum];

	// Initialize frame on first fragment
	if (pf.fragmentCount == 0) {
		pf.fragmentCount = fragCount;
		pf.fragments.resize(fragCount);
		pf.width  = videoData.width;
		pf.height = videoData.height;
		pf.codec  = static_cast<VideoCodec>(videoData.codec);
	}

	// OR keyframe flag (UDP fragments may arrive out of order)
	pf.isKeyFrame |= videoData.isKeyFrame;

	// Store fragment (copy once per fragment, unavoidable unless lifetime guaranteed)
	if (pf.fragments[fragIdx].isEmpty()) {
		pf.fragments[fragIdx] = QByteArray(reinterpret_cast< const char * >(videoData.payload.data()),
										   static_cast< int >(videoData.payload.size()));
	}

	// Early exit until complete
	for (const QByteArray &frag : pf.fragments) {
		if (frag.isEmpty())
			return;
	}

	// Compute total size once
	size_t totalSize = 0;
	for (const QByteArray &frag : pf.fragments)
		totalSize += static_cast< size_t >(frag.size());

	QByteArray complete;
	complete.resize(static_cast< int >(totalSize));

	char *dst = complete.data();
	for (const QByteArray &frag : pf.fragments) {
		memcpy(dst, frag.constData(), static_cast< size_t >(frag.size()));
		dst += frag.size();
	}

	// Capture frame metadata
	const quint32 fw                    = pf.width;
	const quint32 fh                    = pf.height;
	const bool isKeyFrm                 = pf.isKeyFrame;
	const VideoCodec codec              = pf.codec;

	// Cleanup old frames for this session
	std::map< unsigned long long, PendingFrame > &senderMap = m_fragmentBuffer[session];

	// Using auto here as type because this is an iterator
	for (auto it = senderMap.begin(); it != senderMap.end();) {
		if (it->first <= frameNum)
			it = senderMap.erase(it);
		else
			++it;
	}

	decodeCompleteFrame(session, complete, fw, fh, isKeyFrm, codec);
#endif
}

void ScreenShareReceiver::resetSender(quint32 senderSession) {
#ifdef USE_SCREEN_SHARING
	m_fragmentBuffer.erase(senderSession);
	destroyDecoder(senderSession);
#else
	Q_UNUSED(senderSession);
#endif
}

#ifdef USE_SCREEN_SHARING
bool ScreenShareReceiver::ensureDecoder(quint32 session, VideoCodec codec) {
	if (m_decoders.count(session) && m_decoders[session].codecCtx)
		return true;

	const AVCodecID avCodecId = codecIdForProtoCodec(codec);
	if (avCodecId == AV_CODEC_ID_NONE)
		return false;

	// Try hardware decoder first for supported codecs
	const AVCodec *codecPtr = nullptr;
	if (codec == VideoCodec::H264 || codec == VideoCodec::HEVC) {
		// Try hardware decoders
#ifdef Q_OS_MAC
		if (codec == VideoCodec::H264) {
			codecPtr = avcodec_find_decoder_by_name("h264_videotoolbox");
		} else if (codec == VideoCodec::HEVC) {
			codecPtr = avcodec_find_decoder_by_name("hevc_videotoolbox");
		}
#elif defined(Q_OS_LINUX)
		if (codec == VideoCodec::H264) {
			codecPtr = avcodec_find_decoder_by_name("h264_vaapi");
			if (!codecPtr) codecPtr = avcodec_find_decoder_by_name("h264_nvdec");
			if (!codecPtr) codecPtr = avcodec_find_decoder_by_name("h264_qsv");
		} else if (codec == VideoCodec::HEVC) {
			codecPtr = avcodec_find_decoder_by_name("hevc_vaapi");
			if (!codecPtr) codecPtr = avcodec_find_decoder_by_name("hevc_nvdec");
			if (!codecPtr) codecPtr = avcodec_find_decoder_by_name("hevc_qsv");
		}
#elif defined(Q_OS_WIN)
		if (codec == VideoCodec::H264) {
			codecPtr = avcodec_find_decoder_by_name("h264_d3d11va");
		} else if (codec == VideoCodec::HEVC) {
			codecPtr = avcodec_find_decoder_by_name("hevc_d3d11va");
		}
#endif
	}

	// Fall back to software decoder
	if (!codecPtr) {
		codecPtr = avcodec_find_decoder(avCodecId);
	}

	if (!codecPtr)
		return false;

	DecoderState ds;
	ds.codec    = codec;
	ds.codecCtx = avcodec_alloc_context3(codecPtr);
	if (!ds.codecCtx)
		return false;

	// Enable low-latency decoding
	ds.codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ds.codecCtx->flags2 |= AV_CODEC_FLAG2_CHUNKS;

	if (avcodec_open2(ds.codecCtx, codecPtr, nullptr) < 0) {
		avcodec_free_context(&ds.codecCtx);
		return false;
	}

	ds.frame            = av_frame_alloc();
	ds.packet           = av_packet_alloc();
	m_decoders[session] = ds;

	Global::get().l->log(Log::Information, tr("Initialized %1 decoder for session %2 (%3)")
						 .arg(QString::fromLatin1(codecName(codec))).arg(session).arg(QString::fromLatin1(codecPtr->name)));
	return true;
}

void ScreenShareReceiver::destroyDecoder(quint32 session) {
	auto it = m_decoders.find(session);
	if (it == m_decoders.end())
		return;

	DecoderState &ds = it->second;
	if (ds.swsCtx) {
		sws_freeContext(ds.swsCtx);
	}
	if (ds.frame) {
		av_frame_free(&ds.frame);
	}
	if (ds.packet) {
		av_packet_free(&ds.packet);
	}
	if (ds.codecCtx) {
		avcodec_free_context(&ds.codecCtx);
	}
	m_decoders.erase(it);
}

void ScreenShareReceiver::decodeCompleteFrame(quint32 session, const QByteArray &encodedData, quint32 /*width*/,
											  quint32 /*height*/, bool isKeyFrame, VideoCodec codec) {
	if (!ensureDecoder(session, codec))
		return;

	DecoderState &ds = m_decoders[session];

	// Drop non-keyframes until the decoder has seen at least one IDR.
	// Without SPS/PPS (which come with the keyframe) the decoder can't
	// reference picture parameters and emits "non-existing PPS" errors.
	if (!ds.gotKeyFrame && !isKeyFrame)
		return;

	if (isKeyFrame) {
		// Flush any buffered decoder state from a previous stream so the new
		// IDR is treated as a clean start.
		avcodec_flush_buffers(ds.codecCtx);
		ds.gotKeyFrame = true;
	}

	av_packet_unref(ds.packet);
	ds.packet->data = reinterpret_cast< uint8_t * >(const_cast< char * >(encodedData.constData()));
	ds.packet->size = static_cast< int >(encodedData.size());

	if (avcodec_send_packet(ds.codecCtx, ds.packet) < 0) {
		// Packet was rejected (e.g. corrupted reference frame from UDP loss).
		// Flush and wait for the next keyframe so we don't propagate corruption.
		avcodec_flush_buffers(ds.codecCtx);
		ds.gotKeyFrame = false;
		return;
	}

	while (avcodec_receive_frame(ds.codecCtx, ds.frame) == 0) {
		const int dw = ds.frame->width;
		const int dh = ds.frame->height;

		// (Re-)create the sws context if dimensions changed.
		if (!ds.swsCtx || ds.swsWidth != dw || ds.swsHeight != dh) {
			if (ds.swsCtx)
				sws_freeContext(ds.swsCtx);
			ds.swsCtx = sws_getContext(dw, dh, static_cast< AVPixelFormat >(ds.frame->format), dw, dh, AV_PIX_FMT_RGBA,
									   SWS_BILINEAR, nullptr, nullptr, nullptr);
			ds.swsWidth  = dw;
			ds.swsHeight = dh;
		}
		if (!ds.swsCtx)
			continue;

		QImage img(dw, dh, QImage::Format_RGBA8888);
		uint8_t *dstData[1] = { img.bits() };
		int dstStride[1]    = { static_cast< int >(img.bytesPerLine()) };
		sws_scale(ds.swsCtx, ds.frame->data, ds.frame->linesize, 0, dh, dstData, dstStride);

		emit frameDecoded(session, img);
	}
}
#endif