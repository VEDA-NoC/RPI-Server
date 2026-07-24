#include "rtsps/channel_ingest.h"

#include <arpa/inet.h>
#include <gst/gst.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "rtsps/storage_manager.h"

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
    require(pipeline.find("teardown-timeout=2000000000") != std::string::npos,
            "RTSP source does not allow enough time to send TEARDOWN");
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

void test_segment_filename_uses_each_fragment_start_utc() {
    rtsps::ChannelIngestConfig config;
    config.output_dir = "/tmp/recordings/ch1";
    config.channel_id = 1;

    const std::string first = rtsps::build_segment_path(config, 0);
    const std::string second = rtsps::build_segment_path(config, 60001);
    const std::string collision = rtsps::build_segment_path(config, 60001, 1);

    require(first.find("ch1_19700101T000000.000Z.mp4") != std::string::npos,
            "first fragment filename does not contain its UTC start");
    require(second.find("ch1_19700101T000100.001Z.mp4") != std::string::npos,
            "second fragment filename reused the ingest start UTC");
    require(collision.find("ch1_19700101T000100.001Z_001.mp4") != std::string::npos,
            "same-UTC filename collision does not receive a suffix");
}

void test_storage_check_does_not_create_camera_channel_zero_directory() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("rpi-vms-storage-test-" + std::to_string(unique));
    std::filesystem::create_directories(root);

    rtsps::Logger logger(rtsps::LogLevel::Error);
    rtsps::StorageManager storage(root.string(), 0, false, logger);
    const rtsps::StorageStatus status = storage.check();

    require(status.state == rtsps::StorageState::Ready, "temporary storage did not become ready");
    require(std::filesystem::is_directory(root / "recordings"), "recordings root was not created");
    require(!std::filesystem::exists(root / "recordings" / "ch0"), "storage check recreated forbidden ch0");
    std::filesystem::remove_all(root);
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

void test_stalled_rtsp_startup_times_out_and_reconnects() {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listener >= 0, "failed to create stalled RTSP listener");

    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "failed to bind stalled RTSP listener");
    require(::listen(listener, 8) == 0, "failed to listen for stalled RTSP clients");
    socklen_t address_size = sizeof(address);
    require(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) == 0,
            "failed to read stalled RTSP listener port");

    std::atomic<bool> server_running{true};
    std::thread server([&]() {
        std::vector<int> clients;
        while (server_running.load()) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listener, &read_fds);
            timeval timeout{0, 50000};
            if (::select(listener + 1, &read_fds, nullptr, nullptr, &timeout) > 0) {
                const int client = ::accept(listener, nullptr, nullptr);
                if (client >= 0) clients.push_back(client);
            }
        }
        for (const int client : clients) ::close(client);
    });

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("rpi-vms-stalled-ingest-test-" + std::to_string(unique));
    std::filesystem::create_directories(root);

    rtsps::ChannelIngestStats stats;
    {
        rtsps::Logger logger(rtsps::LogLevel::Error);
        rtsps::RecordingIndex index((root / "media.db").string(), logger);
        index.open();
        index.initialize_schema();

        rtsps::ChannelIngestConfig config;
        config.rtsp_location = "rtsp://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/stalled";
        config.camera_user = "unit-user";
        config.camera_password = "unit-password";
        config.output_dir = (root / "recordings").string();
        config.startup_timeout_ms = 200;
        config.reconnect_delay_ms = 10;

        std::atomic<bool> running{true};
        rtsps::ChannelIngest ingest(config, index, logger, running);
        std::thread worker([&ingest]() { ingest.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        running.store(false);
        worker.join();
        stats = ingest.stats();
    }

    server_running.store(false);
    server.join();
    ::close(listener);
    std::filesystem::remove_all(root);

    require(stats.pipeline_errors >= 1, "stalled RTSP startup did not time out");
    require(stats.reconnects >= 1, "stalled RTSP startup did not enter reconnect state");
    require(stats.connection_attempts >= 2, "stalled RTSP startup did not retry the same ingest");
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    test_product_topology();
    test_segment_filename_uses_each_fragment_start_utc();
    test_storage_check_does_not_create_camera_channel_zero_directory();
    test_slow_live_branch_does_not_block_recording();
    test_pipeline_error_reconnects_without_shutdown_drain();
    test_stalled_rtsp_startup_times_out_and_reconnects();
    std::cout << "channel_ingest_test: PASS\n";
    return 0;
}
