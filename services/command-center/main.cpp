#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <random>
#include <string>
#include <memory>

#include "dds/dds.h"
#include "CombatMessages.h"

// OpenTelemetry SDK
#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/sdk/resource/resource.h"

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;
namespace resource = opentelemetry::sdk::resource;

#define SERVICE_NAME "command-center"

static volatile sig_atomic_t running = 1;

static const char* MISSION_TYPES[] = {"RECON", "STRIKE", "SUPPLY", "EVAC"};
static const char* PRIORITIES[] = {"LOW", "MEDIUM", "HIGH", "CRITICAL"};
static const char* ZONES[] = {"Alpha", "Bravo", "Charlie", "Delta"};

void init_tracer() {
    otlp::OtlpHttpExporterOptions opts;
    opts.url = "http://localhost:4318/v1/traces";

    auto exporter = otlp::OtlpHttpExporterFactory::Create(opts);
    auto processor = trace_sdk::SimpleSpanProcessorFactory::Create(std::move(exporter));

    auto resource_attrs = resource::Resource::Create({
        {"service.name", SERVICE_NAME},
        {"service.version", "1.0.0"}
    });

    auto provider = trace_sdk::TracerProviderFactory::Create(std::move(processor), resource_attrs);
    trace_api::Provider::SetTracerProvider(std::move(provider));

    printf("[TRACING] OpenTelemetry initialized for %s\n", SERVICE_NAME);
}

void shutdown_tracer() {
    std::shared_ptr<trace_api::TracerProvider> none;
    trace_api::Provider::SetTracerProvider(none);
}

void handle_signal(int sig) { running = 0; }

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[%s] Starting command center...\n", SERVICE_NAME);

    init_tracer();
    auto tracer = trace_api::Provider::GetTracerProvider()->GetTracer(SERVICE_NAME, "1.0.0");

    // DDS setup
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) {
        fprintf(stderr, "Failed to create participant!\n");
        return 1;
    }

    dds_entity_t topic = dds_create_topic(participant, &combat_MissionOrder_desc,
                                          "MissionOrderTopic", NULL, NULL);
    if (topic < 0) {
        fprintf(stderr, "Failed to create topic!\n");
        return 1;
    }

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 100);

    dds_entity_t writer = dds_create_writer(participant, topic, qos, NULL);
    dds_delete_qos(qos);

    if (writer < 0) {
        fprintf(stderr, "Failed to create writer!\n");
        return 1;
    }

    printf("[%s] DDS connected, waiting for discovery...\n", SERVICE_NAME);
    sleep(3);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> mission_dis(0, 3);
    std::uniform_int_distribution<> priority_dis(0, 3);
    std::uniform_int_distribution<> zone_dis(0, 3);
    std::uniform_real_distribution<> lat_dis(35.0, 42.0);
    std::uniform_real_distribution<> lon_dis(26.0, 45.0);
    std::uniform_int_distribution<> cmd_dis(1, 5);

    int sequence = 0;

    printf("[%s] Command center operational!\n", SERVICE_NAME);

    while (running) {
        // Start span - OTel SDK handles everything automatically
        auto span = tracer->StartSpan("issue-mission");
        auto scope = tracer->WithActiveSpan(span);

        // Extract trace context from span for DDS message header
        auto span_context = span->GetContext();
        char trace_id_hex[33] = {0};
        char span_id_hex[17] = {0};
        span_context.trace_id().ToLowerBase16({trace_id_hex, 32});
        span_context.span_id().ToLowerBase16({span_id_hex, 16});

        char mission_id[64];
        snprintf(mission_id, sizeof(mission_id), "MSN-%ld-%d", time(NULL), sequence);

        const char* mission_type = MISSION_TYPES[mission_dis(gen)];
        const char* priority = PRIORITIES[priority_dis(gen)];
        const char* zone = ZONES[zone_dis(gen)];

        // Set span attributes
        span->SetAttribute("mission.id", mission_id);
        span->SetAttribute("mission.type", mission_type);
        span->SetAttribute("mission.priority", priority);
        span->SetAttribute("mission.zone", zone);
        span->SetAttribute("mission.sequence", sequence);

        // Create DDS message with trace context in header
        combat_MissionOrder msg;
        memset(&msg, 0, sizeof(msg));

        // TRACE CONTEXT EMBEDDED IN MESSAGE HEADER
        msg.trace_ctx.trace_id = trace_id_hex;
        msg.trace_ctx.span_id = span_id_hex;
        msg.trace_ctx.parent_span_id = (char*)"";
        msg.trace_ctx.trace_flags = 1;

        msg.source_service = (char*)SERVICE_NAME;
        msg.timestamp_ns = time(NULL) * 1000000000LL;
        msg.sequence_num = sequence;

        msg.mission_id = mission_id;
        msg.mission_type = (char*)mission_type;
        msg.priority = (char*)priority;
        msg.target_zone = (char*)zone;
        msg.target_lat = lat_dis(gen);
        msg.target_lon = lon_dis(gen);

        char cmd_id[16];
        snprintf(cmd_id, sizeof(cmd_id), "CMD-%d", cmd_dis(gen));
        msg.commander_id = cmd_id;

        dds_return_t ret = dds_write(writer, &msg);

        if (ret >= 0) {
            printf("[ORDER] %s | Zone: %s | Priority: %s | ID: %s | trace: %.8s\n",
                   mission_type, zone, priority, mission_id, trace_id_hex);
            span->SetStatus(trace_api::StatusCode::kOk);
        } else {
            span->SetStatus(trace_api::StatusCode::kError, "DDS write failed");
        }

        span->End();
        sequence++;
        sleep(3);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    shutdown_tracer();
    dds_delete(participant);

    return 0;
}
