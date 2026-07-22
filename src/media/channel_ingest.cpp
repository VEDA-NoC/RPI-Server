#include "rtsps/channel_ingest.h"

#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace rtsps {
namespace {

struct CallbackContext {
    const ChannelIngestConfig& config;
    RecordingIndex& index;
    Logger& logger;
    LiveFrameSink* live_sink;
    std::atomic<bool>& first_buffer_seen;
    std::atomic<std::uint64_t>& first_buffers;
    std::atomic<std::uint64_t>& live_frames;
    std::atomic<std::uint64_t>& live_drops;
};

std::string quote_for_gst_parse(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
        }
        out += ch;
    }
    out += '"';
    return out;
}

std::string redact_rtsp_userinfo(std::string text) {
    constexpr const char* schemes[] = {"rtsp://", "rtsps://"};
    for (const char* scheme : schemes) {
        const std::string scheme_text(scheme);
        std::size_t search_from = 0;
        while (true) {
            const std::size_t scheme_pos = text.find(scheme_text, search_from);
            if (scheme_pos == std::string::npos) {
                break;
            }

            const std::size_t userinfo_start = scheme_pos + scheme_text.size();
            const std::size_t authority_end = text.find_first_of("/ \t\r\n\"", userinfo_start);
            const std::size_t separator = text.find('@', userinfo_start);
            if (separator != std::string::npos && (authority_end == std::string::npos || separator < authority_end)) {
                constexpr const char* replacement = "<redacted>";
                text.replace(userinfo_start, separator - userinfo_start, replacement);
                search_from = userinfo_start + std::char_traits<char>::length(replacement) + 1;
            } else {
                search_from = userinfo_start;
            }
        }
    }
    return text;
}

std::int64_t buffer_clock_time(GstClockTime value) {
    return GST_CLOCK_TIME_IS_VALID(value) ? static_cast<std::int64_t>(value) : -1;
}

void handle_element_message(GstMessage* message, CallbackContext& context) {
    const GstStructure* structure = gst_message_get_structure(message);
    if (!structure) {
        return;
    }

    const char* name = gst_structure_get_name(structure);
    const char* location = gst_structure_get_string(structure, "location");
    if (!name || !location) {
        return;
    }

    const std::string message_name(name);
    const std::string path(location);
    if (message_name == "splitmuxsink-fragment-opened") {
        context.index.mark_segment_opened(context.config.channel_id, path, "mp4", to_codec_name(context.config.codec));
    } else if (message_name == "splitmuxsink-fragment-closed") {
        context.index.mark_segment_closed(path);
    } else {
        context.logger.trace("[gst] element message: " + message_name);
    }
}

GstPadProbeReturn on_ingest_buffer(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
    auto& context = *static_cast<CallbackContext*>(user_data);
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    bool expected = false;
    if (context.first_buffer_seen.compare_exchange_strong(expected, true)) {
        context.first_buffers.fetch_add(1);
        GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        const std::int64_t pts_ns = buffer ? buffer_clock_time(GST_BUFFER_PTS(buffer)) : -1;
        context.logger.info(
            "[ingest] state=streaming first_buffer=yes channel_id=" + std::to_string(context.config.channel_id) +
            " camera_channel=" + std::to_string(context.config.camera_channel) + " pts_ns=" + std::to_string(pts_ns));
    }
    return GST_PAD_PROBE_OK;
}

void on_live_handoff(GstElement*, GstBuffer* buffer, GstPad*, gpointer user_data) {
    auto& context = *static_cast<CallbackContext*>(user_data);
    context.live_frames.fetch_add(1);
    if (!context.live_sink || !buffer) {
        return;
    }

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        context.logger.warn("[live] state=map_error channel_id=" + std::to_string(context.config.channel_id));
        return;
    }

    const LiveEncodedFrameView frame{map.data,
                                     map.size,
                                     buffer_clock_time(GST_BUFFER_PTS(buffer)),
                                     buffer_clock_time(GST_BUFFER_DTS(buffer)),
                                     buffer_clock_time(GST_BUFFER_DURATION(buffer)),
                                     !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT),
                                     context.config.channel_id,
                                     context.config.codec};
    try {
        context.live_sink->on_frame(frame);
    } catch (const std::exception& ex) {
        context.logger.error(std::string("[live] state=sink_error message=") + ex.what());
    } catch (...) {
        context.logger.error("[live] state=sink_error message=unknown");
    }
    gst_buffer_unmap(buffer, &map);
}

void on_live_queue_overrun(GstElement*, gpointer user_data) {
    auto& context = *static_cast<CallbackContext*>(user_data);
    const std::uint64_t drops = context.live_drops.fetch_add(1) + 1;
    if (drops == 1 || drops % 100 == 0) {
        context.logger.warn("[live] state=dropping policy=latest channel_id=" +
                            std::to_string(context.config.channel_id) + " drop_events=" + std::to_string(drops));
    }
}

void configure_source(GstElement* pipeline, const ChannelIngestConfig& config) {
    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "camera_source");
    if (!source) {
        throw std::runtime_error("pipeline is missing camera_source");
    }
    g_object_set(source, "location", config.rtsp_location.c_str(), "user-id", config.camera_user.c_str(), "user-pw",
                 config.camera_password.c_str(), nullptr);
    gst_object_unref(source);
}

void connect_callbacks(GstElement* pipeline, CallbackContext& context) {
    GstElement* tee = gst_bin_get_by_name(GST_BIN(pipeline), "ingest_tee");
    GstElement* live_queue = gst_bin_get_by_name(GST_BIN(pipeline), "live_queue");
    GstElement* live_sink = gst_bin_get_by_name(GST_BIN(pipeline), "live_test_sink");
    if (!tee || !live_queue || !live_sink) {
        if (tee) gst_object_unref(tee);
        if (live_queue) gst_object_unref(live_queue);
        if (live_sink) gst_object_unref(live_sink);
        throw std::runtime_error("pipeline is missing a ChannelIngest branch element");
    }

    GstPad* tee_sink = gst_element_get_static_pad(tee, "sink");
    if (!tee_sink) {
        gst_object_unref(tee);
        gst_object_unref(live_queue);
        gst_object_unref(live_sink);
        throw std::runtime_error("ingest_tee is missing its sink pad");
    }
    gst_pad_add_probe(tee_sink, GST_PAD_PROBE_TYPE_BUFFER, on_ingest_buffer, &context, nullptr);
    g_signal_connect(live_queue, "overrun", G_CALLBACK(on_live_queue_overrun), &context);
    g_signal_connect(live_sink, "handoff", G_CALLBACK(on_live_handoff), &context);

    gst_object_unref(tee_sink);
    gst_object_unref(tee);
    gst_object_unref(live_queue);
    gst_object_unref(live_sink);
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

std::string to_gst_encoding_name(VideoCodec codec) { return codec == VideoCodec::H265 ? "H265" : "H264"; }

std::string to_codec_name(VideoCodec codec) { return codec == VideoCodec::H265 ? "h265" : "h264"; }

std::string build_channel_ingest_pipeline_description(const ChannelIngestConfig& config,
                                                      const std::string& location_pattern) {
    const std::string encoding = to_gst_encoding_name(config.codec);
    const std::string depay = config.codec == VideoCodec::H264 ? "rtph264depay" : "rtph265depay";
    const std::string parse =
        config.codec == VideoCodec::H264 ? "h264parse config-interval=-1" : "h265parse config-interval=-1";
    const std::string alignment_caps =
        config.codec == VideoCodec::H264 ? "video/x-h264,alignment=au" : "video/x-h265,alignment=au";
    const std::int64_t segment_ns = config.segment_seconds * 1000000000LL;

    std::ostringstream pipeline;
    pipeline << "rtspsrc name=camera_source protocols=tcp latency=" << config.latency_ms << ' '
             << "camera_source. ! application/x-rtp,media=video,encoding-name=" << encoding << " ! " << depay << " ! "
             << parse << " ! " << alignment_caps << " ! tee name=ingest_tee "
             << "ingest_tee. ! queue name=recording_queue leaky=no ! "
             << "splitmuxsink name=recording_sink async-finalize=true muxer-factory=mp4mux max-size-time=" << segment_ns
             << " location=" << quote_for_gst_parse(location_pattern) << ' '
             << "ingest_tee. ! queue name=live_queue leaky=downstream max-size-buffers="
             << config.live_queue_max_buffers << " max-size-bytes=0 max-size-time=0 ! "
             << "fakesink name=live_test_sink signal-handoffs=true sync=false async=false";
    return pipeline.str();
}

ChannelIngest::ChannelIngest(ChannelIngestConfig config, RecordingIndex& index, Logger& logger,
                             std::atomic<bool>& running, LiveFrameSink* live_sink)
    : config_(std::move(config)), index_(index), logger_(logger), running_(running), live_sink_(live_sink) {
    if (config_.camera_channel < 0 || config_.camera_channel > 3) {
        throw std::runtime_error("camera_channel out of range (0-3)");
    }
    if (config_.channel_id < 1 || config_.channel_id > 4) {
        throw std::runtime_error("channel_id out of range (1-4)");
    }
    if (config_.live_queue_max_buffers == 0) {
        throw std::runtime_error("live_queue_max_buffers must be at least 1");
    }
}

int ChannelIngest::run() {
    int argc = 0;
    char** argv = nullptr;
    gst_init(&argc, &argv);

    while (running_.load()) {
        const std::uint64_t attempt = connection_attempts_.fetch_add(1) + 1;
        first_buffer_seen_.store(false);
        logger_.info("[ingest] state=connecting channel_id=" + std::to_string(config_.channel_id) + " camera_channel=" +
                     std::to_string(config_.camera_channel) + " attempt=" + std::to_string(attempt));

        const AttemptResult result = run_attempt(attempt);
        if (!running_.load() || result == AttemptResult::Stopped) {
            break;
        }

        const std::uint64_t reconnect = reconnects_.fetch_add(1) + 1;
        const std::string reason = result == AttemptResult::PipelineError ? "pipeline_error" : "eos";
        logger_.warn("[ingest] state=reconnecting channel_id=" + std::to_string(config_.channel_id) +
                     " reason=" + reason + " reconnect=" + std::to_string(reconnect) +
                     " delay_ms=" + std::to_string(config_.reconnect_delay_ms));
        if (!wait_before_reconnect()) {
            break;
        }
    }

    const ChannelIngestStats final_stats = stats();
    logger_.info("[ingest] state=stopped channel_id=" + std::to_string(config_.channel_id) +
                 " attempts=" + std::to_string(final_stats.connection_attempts) +
                 " reconnects=" + std::to_string(final_stats.reconnects) + " first_buffers=" +
                 std::to_string(final_stats.first_buffers) + " live_frames=" + std::to_string(final_stats.live_frames) +
                 " live_drop_events=" + std::to_string(final_stats.live_drops));
    return 0;
}

ChannelIngest::AttemptResult ChannelIngest::run_attempt(std::uint64_t attempt) {
    GError* parse_error = nullptr;
    const std::string pipeline_description =
        build_channel_ingest_pipeline_description(config_, make_location_pattern(attempt));
    logger_.debug("[gst] pipeline: " + pipeline_description);

    GstElement* pipeline = gst_parse_launch(pipeline_description.c_str(), &parse_error);
    if (!pipeline) {
        std::string message = parse_error ? parse_error->message : "unknown gst_parse_launch error";
        if (parse_error) g_error_free(parse_error);
        throw std::runtime_error(redact_rtsp_userinfo("failed to create pipeline: " + message));
    }
    if (parse_error) {
        const std::string message = parse_error->message;
        g_error_free(parse_error);
        gst_object_unref(pipeline);
        throw std::runtime_error(redact_rtsp_userinfo("failed to fully create pipeline: " + message));
    }

    GstBus* bus = nullptr;
    CallbackContext context{config_,        index_,       logger_,    live_sink_, first_buffer_seen_,
                            first_buffers_, live_frames_, live_drops_};
    try {
        configure_source(pipeline, config_);
        connect_callbacks(pipeline, context);
        bus = gst_element_get_bus(pipeline);
    } catch (...) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) gst_object_unref(bus);
        gst_object_unref(pipeline);
        throw;
    }

    AttemptResult result = AttemptResult::Stopped;
    bool received_eos = false;
    const GstStateChangeReturn state_result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (state_result == GST_STATE_CHANGE_FAILURE) {
        pipeline_errors_.fetch_add(1);
        logger_.error("[ingest] state=pipeline_error channel_id=" + std::to_string(config_.channel_id) +
                      " message=failed_to_enter_playing");
        result = AttemptResult::PipelineError;
    }

    try {
        while (result == AttemptResult::Stopped && running_.load()) {
            GstMessage* message = gst_bus_timed_pop_filtered(
                bus, 500 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_ELEMENT));
            if (!message) {
                continue;
            }

            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* gst_error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &gst_error, &debug);
                pipeline_errors_.fetch_add(1);
                logger_.error(redact_rtsp_userinfo(std::string("[ingest] state=pipeline_error channel_id=") +
                                                   std::to_string(config_.channel_id) +
                                                   " message=" + (gst_error ? gst_error->message : "unknown")));
                if (debug) logger_.debug(redact_rtsp_userinfo(std::string("[gst] debug: ") + debug));
                if (gst_error) g_error_free(gst_error);
                if (debug) g_free(debug);
                result = AttemptResult::PipelineError;
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
                eos_events_.fetch_add(1);
                logger_.warn("[ingest] state=eos channel_id=" + std::to_string(config_.channel_id));
                received_eos = true;
                result = AttemptResult::EndOfStream;
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT) {
                handle_element_message(message, context);
            }
            gst_message_unref(message);
        }

        if (result == AttemptResult::Stopped && !received_eos &&
            gst_element_send_event(pipeline, gst_event_new_eos())) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                GstMessage* message = gst_bus_timed_pop_filtered(
                    bus, 500 * GST_MSECOND,
                    static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_ELEMENT));
                if (!message) continue;
                const GstMessageType type = GST_MESSAGE_TYPE(message);
                if (type == GST_MESSAGE_ELEMENT) {
                    handle_element_message(message, context);
                    gst_message_unref(message);
                    continue;
                }
                gst_message_unref(message);
                break;
            }
        }
    } catch (...) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        throw;
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return running_.load() ? result : AttemptResult::Stopped;
}

ChannelIngestStats ChannelIngest::stats() const {
    return ChannelIngestStats{connection_attempts_.load(), reconnects_.load(), first_buffers_.load(),
                              pipeline_errors_.load(),     eos_events_.load(), live_frames_.load(),
                              live_drops_.load()};
}

std::string ChannelIngest::make_location_pattern(std::uint64_t attempt) const {
    std::filesystem::create_directories(config_.output_dir);

    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);

    std::ostringstream filename;
    filename << "ch" << config_.channel_id << '_' << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    if (attempt > 1) {
        filename << "_r" << std::setfill('0') << std::setw(3) << attempt - 1;
    }
    filename << "_%05d.mp4";
    return (std::filesystem::path(config_.output_dir) / filename.str()).string();
}

bool ChannelIngest::wait_before_reconnect() const {
    int remaining_ms = config_.reconnect_delay_ms;
    while (running_.load() && remaining_ms > 0) {
        const int wait_ms = std::min(remaining_ms, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        remaining_ms -= wait_ms;
    }
    return running_.load();
}

}  // namespace rtsps
