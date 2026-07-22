#include "rtsps/channel_ingest.h"

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void count_handoff(GstElement*, GstBuffer*, GstPad*, gpointer user_data) {
    static_cast<std::atomic<unsigned int>*>(user_data)->fetch_add(1);
}

void count_overrun(GstElement*, gpointer user_data) {
    static_cast<std::atomic<unsigned int>*>(user_data)->fetch_add(1);
}

void test_product_topology() {
    rtsps::ChannelIngestConfig config;
    config.rtsp_location = "rtsp://camera.invalid/0/profile2/media.smp";
    config.camera_user = "unit-user";
    config.camera_password = "unit-password-secret";
    config.codec = rtsps::VideoCodec::H264;
    config.live_queue_max_buffers = 1;

    const std::string pipeline = rtsps::build_channel_ingest_pipeline_description(config, "/tmp/ch1_%05d.mp4");
    require(pipeline.find("rtsp://") == std::string::npos, "pipeline description contains the camera URI");
    require(pipeline.find(config.camera_user) == std::string::npos, "pipeline description contains the camera user");
    require(pipeline.find(config.camera_password) == std::string::npos,
            "pipeline description contains the camera password");
    require(pipeline.find("rtph264depay ! h264parse config-interval=-1 ! video/x-h264,alignment=au ! tee") !=
                std::string::npos,
            "tee is not placed after H.264 depay/parse/AU alignment");
    require(pipeline.find("queue name=recording_queue leaky=no") != std::string::npos,
            "recording queue is not explicitly non-leaky");
    require(pipeline.find("queue name=live_queue leaky=downstream max-size-buffers=1 max-size-bytes=0 "
                          "max-size-time=0") != std::string::npos,
            "live queue does not implement the latest-buffer policy");

    config.codec = rtsps::VideoCodec::H265;
    const std::string h265_pipeline = rtsps::build_channel_ingest_pipeline_description(config, "/tmp/ch1_%05d.mp4");
    require(h265_pipeline.find("rtph265depay ! h265parse config-interval=-1 ! video/x-h265,alignment=au ! tee") !=
                std::string::npos,
            "H.265 topology does not tee parsed access units");
}

void test_slow_live_branch_does_not_block_recording() {
    constexpr unsigned int kBufferCount = 400;
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(
        "fakesrc num-buffers=400 sizetype=fixed sizemax=1024 filltype=pattern ! tee name=t "
        "t. ! queue name=recording_queue leaky=no ! "
        "fakesink name=recording_sink signal-handoffs=true sync=false async=false "
        "t. ! queue name=live_queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
        "identity sleep-time=20000 ! fakesink sync=false async=false",
        &error);
    require(pipeline != nullptr, error ? error->message : "failed to create queue isolation pipeline");
    require(error == nullptr, error ? error->message : "partial queue isolation pipeline");

    GstElement* recording_sink = gst_bin_get_by_name(GST_BIN(pipeline), "recording_sink");
    GstElement* live_queue = gst_bin_get_by_name(GST_BIN(pipeline), "live_queue");
    require(recording_sink != nullptr && live_queue != nullptr, "test pipeline elements are missing");

    std::atomic<unsigned int> recording_buffers{0};
    std::atomic<unsigned int> live_overruns{0};
    g_signal_connect(recording_sink, "handoff", G_CALLBACK(count_handoff), &recording_buffers);
    g_signal_connect(live_queue, "overrun", G_CALLBACK(count_overrun), &live_overruns);

    GstBus* bus = gst_element_get_bus(pipeline);
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
            "test pipeline failed to enter PLAYING");
    GstMessage* terminal = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND,
                                                      static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    require(terminal != nullptr, "test pipeline did not finish within 10 seconds");
    require(GST_MESSAGE_TYPE(terminal) == GST_MESSAGE_EOS, "test pipeline ended with an error");
    require(recording_buffers.load() == kBufferCount, "non-leaky recording branch lost buffers");
    require(live_overruns.load() > 0, "slow live branch did not exercise the leaky queue");

    gst_message_unref(terminal);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(recording_sink);
    gst_object_unref(live_queue);
    gst_object_unref(pipeline);
}

void test_pipeline_error_reconnects_without_shutdown_drain() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("rpi-vms-channel-ingest-test-" + std::to_string(unique));

    rtsps::ChannelIngestStats stats;
    {
        rtsps::Logger logger(rtsps::LogLevel::Error);
        rtsps::RecordingIndex index((root / "media.db").string(), logger);
        std::filesystem::create_directories(root);
        index.open();
        index.initialize_schema();

        rtsps::ChannelIngestConfig config;
        config.rtsp_location = "rtsp://127.0.0.1:1/unreachable";
        config.camera_user = "unit-user";
        config.camera_password = "unit-password";
        config.output_dir = (root / "recordings").string();
        config.reconnect_delay_ms = 10;

        std::atomic<bool> running{true};
        rtsps::ChannelIngest ingest(config, index, logger, running);
        std::thread worker([&ingest]() { ingest.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        running.store(false);
        worker.join();
        stats = ingest.stats();
    }

    std::filesystem::remove_all(root);
    require(stats.pipeline_errors >= 1, "unreachable RTSP endpoint did not produce a pipeline error");
    require(stats.reconnects >= 1, "pipeline error did not enter reconnect state");
    require(stats.connection_attempts >= 2, "pipeline error did not start another connection attempt");
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    test_product_topology();
    test_slow_live_branch_does_not_block_recording();
    test_pipeline_error_reconnects_without_shutdown_drain();
    std::cout << "channel_ingest_test: PASS\n";
    return 0;
}
