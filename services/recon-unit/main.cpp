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
#include "opentelemetry/trace/context.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/context/propagation/global_propagator.h"

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;
namespace resource = opentelemetry::sdk::resource;

#define SERVICE_NAME "recon-unit"

static volatile sig_atomic_t running = 1;
static const char* THREAT_LEVELS[] = {"NONE", "LOW", "MEDIUM", "HIGH", "EXTREME"};
static const char* TERRAIN_TYPES[] = {"URBAN", "FOREST", "DESERT", "MOUNTAIN"};

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

    printf("[%s] Starting reconnaissance unit...\n", SERVICE_NAME);

    init_tracer();
    auto tracer = trace_api::Provider::GetTracerProvider()->GetTracer(SERVICE_NAME, "1.0.0");

    // DDS setup
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) { fprintf(stderr, "Failed to create participant!\n"); return 1; }

    dds_entity_t read_topic = dds_create_topic(participant, &combat_MissionOrder_desc,
                                                "MissionOrderTopic", NULL, NULL);
    dds_entity_t write_topic = dds_create_topic(participant, &combat_ReconReport_desc,
                                                 "ReconReportTopic", NULL, NULL);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 100);

    dds_entity_t reader = dds_create_reader(participant, read_topic, qos, NULL);
    dds_entity_t writer = dds_create_writer(participant, write_topic, qos, NULL);
    dds_delete_qos(qos);

    printf("[%s] DDS connected, waiting for discovery...\n", SERVICE_NAME);
    sleep(3);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> confirm_dis(0.0, 1.0);
    std::uniform_int_distribution<> enemy_dis(0, 50);
    std::uniform_int_distribution<> threat_dis(0, 4);
    std::uniform_int_distribution<> terrain_dis(0, 3);
    std::uniform_int_distribution<> unit_dis(1, 5);

    void* samples[10];
    dds_sample_info_t infos[10];
    for (int i = 0; i < 10; i++) samples[i] = combat_MissionOrder__alloc();

    printf("[%s] Recon unit ready, awaiting mission orders...\n", SERVICE_NAME);

    while (running) {
        dds_return_t n = dds_take(reader, samples, infos, 10, 10);

        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (!infos[i].valid_data) continue;

                combat_MissionOrder* order = (combat_MissionOrder*)samples[i];

                // Extract trace context from DDS message header
                std::string trace_id_str = order->trace_ctx.trace_id ? order->trace_ctx.trace_id : "";
                std::string parent_span_str = order->trace_ctx.span_id ? order->trace_ctx.span_id : "";

                // Create parent span context from incoming message
                trace_api::TraceId trace_id;
                trace_api::SpanId parent_span_id;

                if (trace_id_str.length() == 32) {
                    uint8_t trace_id_buf[16];
                    for (int j = 0; j < 16; j++) {
                        sscanf(trace_id_str.c_str() + j*2, "%2hhx", &trace_id_buf[j]);
                    }
                    trace_id = trace_api::TraceId(trace_id_buf);
                }

                if (parent_span_str.length() == 16) {
                    uint8_t span_id_buf[8];
                    for (int j = 0; j < 8; j++) {
                        sscanf(parent_span_str.c_str() + j*2, "%2hhx", &span_id_buf[j]);
                    }
                    parent_span_id = trace_api::SpanId(span_id_buf);
                }

                // Create span context and start child span
                auto parent_ctx = trace_api::SpanContext(trace_id, parent_span_id,
                    trace_api::TraceFlags(trace_api::TraceFlags::kIsSampled), true);

                trace_api::StartSpanOptions opts;
                opts.parent = parent_ctx;

                auto span = tracer->StartSpan("execute-recon", opts);
                auto scope = tracer->WithActiveSpan(span);

                printf("[RECON] Mission: %s | Zone: %s | Priority: %s | trace: %.8s\n",
                       order->mission_type, order->target_zone, order->priority, trace_id_str.c_str());

                span->SetAttribute("mission.id", order->mission_id ? order->mission_id : "");
                span->SetAttribute("mission.type", order->mission_type ? order->mission_type : "");
                span->SetAttribute("mission.zone", order->target_zone ? order->target_zone : "");

                // Simulate reconnaissance duration
                usleep(500000 + (rand() % 1000000));

                // Recon results (80% target confirmed)
                bool target_confirmed = confirm_dis(gen) > 0.20;
                int enemy_count = target_confirmed ? enemy_dis(gen) : 0;
                const char* threat_level = THREAT_LEVELS[threat_dis(gen)];
                const char* terrain = TERRAIN_TYPES[terrain_dis(gen)];

                span->SetAttribute("recon.target_confirmed", target_confirmed);
                span->SetAttribute("recon.enemy_count", enemy_count);
                span->SetAttribute("recon.threat_level", threat_level);
                span->SetAttribute("recon.terrain", terrain);

                // Get new span context for propagation
                auto span_context = span->GetContext();
                char new_span_id_hex[17] = {0};
                span_context.span_id().ToLowerBase16({new_span_id_hex, 16});

                printf("[INTEL] %s | Enemies: %d | Threat: %s | Terrain: %s | trace: %.8s\n",
                       target_confirmed ? "TARGET CONFIRMED" : "TARGET NOT FOUND",
                       enemy_count, threat_level, terrain, trace_id_str.c_str());

                span->SetStatus(target_confirmed ? trace_api::StatusCode::kOk : trace_api::StatusCode::kError);
                span->End();

                // Send recon report via DDS
                combat_ReconReport report;
                memset(&report, 0, sizeof(report));

                // Propagate trace context to next service
                report.trace_ctx.trace_id = (char*)trace_id_str.c_str();
                report.trace_ctx.span_id = new_span_id_hex;
                report.trace_ctx.parent_span_id = (char*)parent_span_str.c_str();
                report.trace_ctx.trace_flags = 1;

                report.source_service = (char*)SERVICE_NAME;
                report.timestamp_ns = time(NULL) * 1000000000LL;
                report.mission_id = order->mission_id;

                char report_id[64];
                snprintf(report_id, sizeof(report_id), "RPT-%ld", time(NULL));
                report.report_id = report_id;

                char unit_id[16];
                snprintf(unit_id, sizeof(unit_id), "UNIT-%d", unit_dis(gen));
                report.unit_id = unit_id;

                report.target_confirmed = target_confirmed;
                report.enemy_count = enemy_count;
                report.threat_level = (char*)threat_level;
                report.terrain_type = (char*)terrain;
                report.intel_details = (char*)"{}";

                dds_write(writer, &report);
            }
        }

        usleep(100000);
    }

    for (int i = 0; i < 10; i++) {
        dds_sample_free(samples[i], &combat_MissionOrder_desc, DDS_FREE_ALL);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    shutdown_tracer();
    dds_delete(participant);
    return 0;
}
