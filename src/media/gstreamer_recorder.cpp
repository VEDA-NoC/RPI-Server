#include "rtsps/gstreamer_recorder.h"

#include <gst/gst.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rtsps {
namespace {

std::string quote_for_gst_parse(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else {
            out += ch;
        }
    }
    out += "\"";
    return out;
}

void handle_element_message(GstMessage* message, RecordingIndex& index, int channel_id, const std::string& codec,
                            Logger& logger) {
    const GstStructure* structure = gst_message_get_structure(message);
    if (!structure) {
        return;
    }

    const char* name = gst_structure_get_name(structure);
    if (!name) {
        return;
    }

    const char* location = gst_structure_get_string(structure, "location");
    if (!location) {
        return;
    }

    const std::string message_name(name);
    const std::string path(location);
    if (message_name == "splitmuxsink-fragment-opened") {
        index.mark_segment_opened(channel_id, path, "mp4", codec);
    } else if (message_name == "splitmuxsink-fragment-closed") {
        index.mark_segment_closed(path);
    } else {
        logger.trace("[gst] element message: " + message_name);
    }
}

}  // namespace

VideoCodec parse_video_codec(const std::string& value) {
    if (value == "h264" || value == "H264") {
        return VideoCodec::H264;
    }
    if (value == "h265" || value == "hevc" || value == "H265" || value == "HEVC") {
        return VideoCodec::H265;
    }
    throw std::runtime_error("unsupported codec: " + value);
}

std::string to_gst_encoding_name(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H264:
            return "H264";
        case VideoCodec::H265:
            return "H265";
    }
    return "H264";
}

std::string to_codec_name(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H264:
            return "h264";
        case VideoCodec::H265:
            return "h265";
    }
    return "h264";
}

GStreamerRecorder::GStreamerRecorder(GStreamerRecorderConfig config, RecordingIndex& index, Logger& logger,
                                     std::atomic<bool>& running)
    : config_(std::move(config)), index_(index), logger_(logger), running_(running) {}

int GStreamerRecorder::run() {
    int argc = 0;
    char** argv = nullptr;
    gst_init(&argc, &argv);

    GError* error = nullptr;
    const std::string pipeline_description = build_pipeline();
    logger_.info("[gst] pipeline: " + pipeline_description);

    GstElement* pipeline = gst_parse_launch(pipeline_description.c_str(), &error);
    if (!pipeline) {
        std::string message = error ? error->message : "unknown gst_parse_launch error";
        if (error) {
            g_error_free(error);
        }
        throw std::runtime_error("failed to create pipeline: " + message);
    }

    GstBus* bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    logger_.info("[gst] recording started");

    int result = 0;
    while (running_.load()) {
        GstMessage* message =
            gst_bus_timed_pop_filtered(bus, 500 * GST_MSECOND,
                                       static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
                                                                   GST_MESSAGE_ELEMENT | GST_MESSAGE_STATE_CHANGED));
        if (!message) {
            continue;
        }

        switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ERROR: {
                GError* gst_error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &gst_error, &debug);
                logger_.error(std::string("[gst] error: ") + (gst_error ? gst_error->message : "unknown"));
                if (debug) {
                    logger_.debug(std::string("[gst] debug: ") + debug);
                }
                if (gst_error) {
                    g_error_free(gst_error);
                }
                if (debug) {
                    g_free(debug);
                }
                result = 1;
                running_.store(false);
                break;
            }
            case GST_MESSAGE_EOS:
                logger_.info("[gst] eos");
                running_.store(false);
                break;
            case GST_MESSAGE_ELEMENT:
                handle_element_message(message, index_, config_.channel_id, to_codec_name(config_.codec), logger_);
                break;
            default:
                break;
        }
        gst_message_unref(message);
    }

    logger_.info("[gst] stopping pipeline");
    if (gst_element_send_event(pipeline, gst_event_new_eos())) {
        GstMessage* eos_message = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_ELEMENT));
        while (eos_message) {
            if (GST_MESSAGE_TYPE(eos_message) == GST_MESSAGE_ELEMENT) {
                handle_element_message(eos_message, index_, config_.channel_id, to_codec_name(config_.codec), logger_);
                gst_message_unref(eos_message);
                eos_message = gst_bus_timed_pop_filtered(
                    bus, 2 * GST_SECOND,
                    static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_ELEMENT));
                continue;
            }
            gst_message_unref(eos_message);
            break;
        }
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    logger_.info("[gst] recording stopped");
    return result;
}

std::string GStreamerRecorder::build_pipeline() const {
    const std::string encoding = to_gst_encoding_name(config_.codec);
    const std::string depay = config_.codec == VideoCodec::H264 ? "rtph264depay" : "rtph265depay";
    const std::string parse =
        config_.codec == VideoCodec::H264 ? "h264parse config-interval=-1" : "h265parse config-interval=-1";
    const std::string location = make_location_pattern();
    const std::int64_t segment_ns = config_.segment_seconds * 1000000000LL;

    std::ostringstream pipeline;
    pipeline << "rtspsrc location=" << quote_for_gst_parse(config_.rtsp_url)
             << " protocols=tcp latency=" << config_.latency_ms << " name=src "
             << "src. ! application/x-rtp,media=video,encoding-name=" << encoding << " ! " << depay << " ! " << parse
             << " ! "
             << "splitmuxsink async-finalize=true muxer-factory=mp4mux max-size-time=" << segment_ns
             << " location=" << quote_for_gst_parse(location);
    return pipeline.str();
}

std::string GStreamerRecorder::make_location_pattern() const {
    std::filesystem::create_directories(config_.output_dir);

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);

    std::ostringstream stamp;
    stamp << std::put_time(&tm, "%Y%m%dT%H%M%SZ");

    const std::string filename = "ch" + std::to_string(config_.channel_id) + "_" + stamp.str() + "_%05d.mp4";
    return (std::filesystem::path(config_.output_dir) / filename).string();
}

}  // namespace rtsps
