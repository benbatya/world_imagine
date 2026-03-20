#include "FrameExtractor.hpp"
#include "util/AsyncJob.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

std::vector<fs::path> FrameExtractor::run(const FrameExtractorConfig& cfg,
                                           AsyncJob& job,
                                           float progressLo,
                                           float progressHi) {
    fs::create_directories(cfg.outputDir);

    // --- Open input ---
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, cfg.videoPath.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("Cannot open video: " + cfg.videoPath.string());
    struct FmtGuard {
        AVFormatContext** p;
        ~FmtGuard() { avformat_close_input(p); }
    } fmtGuard{&fmtCtx};

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
        throw std::runtime_error("Cannot read stream info");

    // --- Find video stream ---
    int videoStream = -1;
    const AVCodec* codec = nullptr;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = static_cast<int>(i);
            codec = avcodec_find_decoder(fmtCtx->streams[i]->codecpar->codec_id);
            break;
        }
    }
    if (videoStream < 0) throw std::runtime_error("No video stream found");
    if (!codec) throw std::runtime_error("Cannot find video decoder");

    // --- Open codec ---
    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) throw std::runtime_error("Cannot allocate codec context");
    struct CodecGuard {
        AVCodecContext* p;
        ~CodecGuard() { avcodec_free_context(&p); }
    } codecGuard{codecCtx};

    avcodec_parameters_to_context(codecCtx, fmtCtx->streams[videoStream]->codecpar);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
        throw std::runtime_error("Cannot open codec");

    int w = codecCtx->width;
    int h = codecCtx->height;

    // --- SwsContext: decode → RGB24 ---
    SwsContext* swsCtx = sws_getContext(w, h, codecCtx->pix_fmt,
                                        w, h, AV_PIX_FMT_RGB24,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx) throw std::runtime_error("Cannot create SwsContext");
    struct SwsGuard {
        SwsContext* p;
        ~SwsGuard() { sws_freeContext(p); }
    } swsGuard{swsCtx};

    // --- Allocate frame buffers ---
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    struct FrameGuard {
        AVFrame* f;
        ~FrameGuard() { av_frame_free(&f); }
    } fg1{frame}, fg2{rgbFrame};

    std::vector<uint8_t> rgbBuf(
        static_cast<size_t>(av_image_get_buffer_size(AV_PIX_FMT_RGB24, w, h, 1)));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize,
                         rgbBuf.data(), AV_PIX_FMT_RGB24, w, h, 1);

    // --- Decode loop ---
    AVPacket* pkt = av_packet_alloc();
    struct PktGuard {
        AVPacket* p;
        ~PktGuard() { av_packet_free(&p); }
    } pktGuard{pkt};

    int64_t totalFrames = fmtCtx->streams[videoStream]->nb_frames;

    std::vector<fs::path> outPaths;
    int64_t frameIndex = 0;
    int savedCount = 0;

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (job.cancelRequested()) {
            av_packet_unref(pkt);
            return {};
        }

        if (pkt->stream_index == videoStream) {
            if (avcodec_send_packet(codecCtx, pkt) == 0) {
                while (avcodec_receive_frame(codecCtx, frame) == 0) {
                    if (frameIndex % cfg.everyNthFrame == 0) {
                        sws_scale(swsCtx, frame->data, frame->linesize,
                                  0, h, rgbFrame->data, rgbFrame->linesize);

                        // OpenCV expects BGR; convert then write JPEG
                        cv::Mat rgb(h, w, CV_8UC3, rgbBuf.data());
                        cv::Mat bgr;
                        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

                        auto outPath = cfg.outputDir / std::format("frame_{:06d}.jpg", savedCount++);
                        cv::imwrite(outPath.string(), bgr);
                        outPaths.push_back(outPath);
                    }
                    ++frameIndex;

                    if (totalFrames > 0) {
                        float frac = static_cast<float>(frameIndex) / static_cast<float>(totalFrames);
                        job.setProgress(progressLo + frac * (progressHi - progressLo));
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    job.setProgress(progressHi);
    return outPaths;
}
