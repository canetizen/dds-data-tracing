#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <vector>
#include <string>
#include <sstream>

#include "traced_dds.hpp"
#include "CombatMessages.h"

TRACED_DDS_TYPE(combat_SourceTrack);
TRACED_DDS_TYPE(combat_TacticalTrack);

#define SERVICE_NAME "track-fusion"
#define FUSION_WINDOW_SEC 3  // Collect tracks for N seconds before fusing

static volatile sig_atomic_t running = 1;

void handle_signal(int sig) { running = 0; }

// Collected track with trace link info
struct CollectedTrack {
    // Numeric data
    int64_t timestamp_ns;
    float position_lat;
    float position_lon;
    float altitude_m;
    float heading_deg;
    float speed_mps;
    float confidence;
    
    // String data (owned copies)
    char sensor_id[32];
    char sensor_type[32];
    char track_id[32];
    char classification[32];
    
    // Trace link
    traced::TraceLink link;
};

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[%s] Starting track fusion service...\n", SERVICE_NAME);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) {
        fprintf(stderr, "Failed to create participant!\n");
        return 1;
    }

    // Reader for source tracks
    auto reader = TRACED_READER(combat_SourceTrack, participant, "SourceTrackTopic");
    
    // Writer for tactical tracks
    auto writer = TRACED_WRITER(combat_TacticalTrack, participant, "TacticalTrackTopic");

    printf("[%s] DDS connected, waiting for discovery...\n", SERVICE_NAME);
    sleep(3);

    std::vector<CollectedTrack> collected_tracks;
    int tactical_track_num = 1;
    time_t last_fusion_time = time(NULL);

    printf("[%s] Fusion service operational - collecting source tracks\n", SERVICE_NAME);

    while (running) {
        // Collect incoming tracks (don't process in callback - just store)
        void* samples[10] = {nullptr};
        dds_sample_info_t infos[10];
        
        dds_return_t n = dds_take(reader.get(), samples, infos, 10, 10);
        
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (!infos[i].valid_data) continue;
                
                combat_SourceTrack* msg = static_cast<combat_SourceTrack*>(samples[i]);
                
                CollectedTrack ct;
                
                // Copy string data
                strncpy(ct.sensor_id, msg->sensor_id ? msg->sensor_id : "", sizeof(ct.sensor_id)-1);
                ct.sensor_id[sizeof(ct.sensor_id)-1] = '\0';
                strncpy(ct.sensor_type, msg->sensor_type ? msg->sensor_type : "", sizeof(ct.sensor_type)-1);
                ct.sensor_type[sizeof(ct.sensor_type)-1] = '\0';
                strncpy(ct.track_id, msg->source_track_id ? msg->source_track_id : "", sizeof(ct.track_id)-1);
                ct.track_id[sizeof(ct.track_id)-1] = '\0';
                strncpy(ct.classification, msg->classification ? msg->classification : "", sizeof(ct.classification)-1);
                ct.classification[sizeof(ct.classification)-1] = '\0';
                
                // Copy numeric data
                ct.timestamp_ns = msg->timestamp_ns;
                ct.position_lat = msg->position_lat;
                ct.position_lon = msg->position_lon;
                ct.altitude_m = msg->altitude_m;
                ct.heading_deg = msg->heading_deg;
                ct.speed_mps = msg->speed_mps;
                ct.confidence = msg->confidence;
                
                // Extract trace link
                ct.link.trace_id = msg->trace_ctx.trace_id ? msg->trace_ctx.trace_id : "";
                ct.link.span_id = msg->trace_ctx.span_id ? msg->trace_ctx.span_id : "";
                ct.link.sensor_id = ct.sensor_id;
                
                collected_tracks.push_back(ct);
                
                printf("[COLLECT] %s track %s | Pos: %.2f, %.2f\n",
                       ct.sensor_type, ct.track_id,
                       ct.position_lat, ct.position_lon);
            }
        }
        
        // Check if it's time to fuse
        time_t now = time(NULL);
        if (now - last_fusion_time >= FUSION_WINDOW_SEC && !collected_tracks.empty()) {
            
            // ========== FUSION PROCESS WITH TRACING ==========
            
            // 1. Extract all trace links from collected tracks
            std::vector<traced::TraceLink> links;
            for (const auto& ct : collected_tracks) {
                links.push_back(ct.link);
            }
            
            // 2. Create root span with links to all source traces
            auto [fuse_span, fuse_scope] = traced::create_linked_span("fuse-tracks", links);
            fuse_span->SetAttribute("fusion.num_sources", (int64_t)collected_tracks.size());
            
            // 3. Receive spans for each sensor (child spans for timing)
            for (const auto& ct : collected_tracks) {
                std::string span_name = std::string("receive-") + ct.sensor_type;
                auto [recv_span, recv_scope] = traced::create_child_span(span_name);
                recv_span->SetAttribute("sensor.id", std::string(ct.sensor_id));
                recv_span->SetAttribute("track.id", std::string(ct.track_id));
                recv_span->SetAttribute("track.confidence", ct.confidence);
                recv_span->End();
            }
            
            // 4. Correlate span
            {
                auto [corr_span, corr_scope] = traced::create_child_span("correlate");
                corr_span->SetAttribute("algorithm", "centroid-fusion");
                
                // Simple fusion: average positions, max confidence
                // (In real system this would be Kalman filter etc.)
                usleep(10000);  // Simulate processing
                
                corr_span->End();
            }
            
            // 5. Publish tactical track
            {
                auto [pub_span, pub_scope] = traced::create_child_span("publish-tactical");
                
                // Build tactical track
                char tac_id[32];
                snprintf(tac_id, sizeof(tac_id), "TT-%03d", tactical_track_num);
                
                // Aggregate data
                float avg_lat = 0, avg_lon = 0, avg_alt = 0;
                float avg_hdg = 0, avg_spd = 0, max_conf = 0;
                std::stringstream sensors_ss, track_ids_ss;
                char best_class_buf[32] = "UNKNOWN";
                
                for (size_t i = 0; i < collected_tracks.size(); i++) {
                    const auto& ct = collected_tracks[i];
                    avg_lat += ct.position_lat;
                    avg_lon += ct.position_lon;
                    avg_alt += ct.altitude_m;
                    avg_hdg += ct.heading_deg;
                    avg_spd += ct.speed_mps;
                    
                    if (ct.confidence > max_conf) {
                        max_conf = ct.confidence;
                        strncpy(best_class_buf, ct.classification, sizeof(best_class_buf)-1);
                        best_class_buf[sizeof(best_class_buf)-1] = '\0';
                    }
                    
                    if (i > 0) {
                        sensors_ss << ",";
                        track_ids_ss << ",";
                    }
                    sensors_ss << ct.sensor_id;
                    track_ids_ss << ct.track_id;
                }
                
                size_t n_tracks = collected_tracks.size();
                avg_lat /= n_tracks;
                avg_lon /= n_tracks;
                avg_alt /= n_tracks;
                avg_hdg /= n_tracks;
                avg_spd /= n_tracks;
                
                std::string sensors_str = sensors_ss.str();
                std::string track_ids_str = track_ids_ss.str();
                
                combat_TacticalTrack tac;
                memset(&tac, 0, sizeof(tac));
                
                tac.fusion_service_id = (char*)SERVICE_NAME;
                tac.timestamp_ns = now * 1000000000LL;
                tac.tactical_track_id = tac_id;
                tac.position_lat = avg_lat;
                tac.position_lon = avg_lon;
                tac.altitude_m = avg_alt;
                tac.heading_deg = avg_hdg;
                tac.speed_mps = avg_spd;
                tac.confidence = max_conf;
                tac.classification = best_class_buf;
                tac.num_sources = (int32_t)n_tracks;
                tac.contributing_sensors = (char*)sensors_str.c_str();
                tac.contributing_track_ids = (char*)track_ids_str.c_str();
                
                pub_span->SetAttribute("tactical.track_id", tac_id);
                pub_span->SetAttribute("tactical.num_sources", (int64_t)n_tracks);
                pub_span->SetAttribute("tactical.confidence", (double)max_conf);
                
                // Write will continue the trace
                if (writer.write(tac, "emit-tactical-track")) {
                    printf("\n[FUSION] ══════════════════════════════════════════\n");
                    printf("[FUSION] Tactical Track: %s\n", tac_id);
                    printf("[FUSION] Sources: %s\n", sensors_str.c_str());
                    printf("[FUSION] Position: %.4f, %.4f | Alt: %.0fm\n", 
                           avg_lat, avg_lon, avg_alt);
                    printf("[FUSION] Classification: %s | Confidence: %.2f\n",
                           best_class_buf, max_conf);
                    printf("[FUSION] ══════════════════════════════════════════\n\n");
                }
                
                pub_span->End();
            }
            
            fuse_span->End();
            
            // Clear for next window
            collected_tracks.clear();
            tactical_track_num++;
            last_fusion_time = now;
        }
        
        usleep(100000);  // 100ms poll interval
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    dds_delete(participant);

    return 0;
}
