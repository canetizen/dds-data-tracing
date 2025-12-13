# DDS Data Tracing Demo

A demonstration project showing distributed tracing across DDS (Data Distribution Service) messages using Eclipse CycloneDDS and OpenTelemetry.

## Overview

This project demonstrates how to implement end-to-end distributed tracing in a DDS-based pub-sub system. Trace context (W3C Trace Context standard) is embedded directly into DDS message headers, enabling correlation of operations across multiple services.

## Architecture

```
┌─────────────────┐     MissionOrderTopic      ┌─────────────────┐
│  command-center │ ─────────────────────────► │   recon-unit    │
│  (Root Spans)   │                            │  (Child Spans)  │
└─────────────────┘                            └────────┬────────┘
                                                        │
                                               ReconReportTopic
                                                        │
                                                        ▼
┌─────────────────┐     SupplyUpdateTopic      ┌─────────────────┐
│ tactical-display│ ◄───────────────────────── │ logistics-depot │
│   (Monitoring)  │                            │  (Child Spans)  │
└─────────────────┘                            └─────────────────┘
        ▲
        │         MissionOrderTopic
        │         ReconReportTopic
        └──────── SupplyUpdateTopic ───────────────────┘
```

## Services

| Service | Role | DDS Topics |
|---------|------|------------|
| **command-center** | Issues mission orders, creates root spans | Publishes: `MissionOrderTopic` |
| **recon-unit** | Executes reconnaissance missions | Subscribes: `MissionOrderTopic`, Publishes: `ReconReportTopic` |
| **logistics-depot** | Manages supply dispatching | Subscribes: `ReconReportTopic`, Publishes: `SupplyUpdateTopic` |
| **tactical-display** | Monitors all operations | Subscribes: All topics |

## Project Structure

```
dds-data-tracing/
├── docker-compose.yml          # Service orchestration
├── Dockerfile                  # Multi-stage build (IDL gen → OTel SDK → App → Runtime)
├── Makefile                    # Build shortcuts
├── shared/
│   ├── CombatMessages.idl      # DDS message type definitions with TraceContext
│   └── cyclonedds.xml          # CycloneDDS configuration
└── services/
    ├── command-center/         # Mission order issuer
    │   ├── main.cpp
    │   └── CMakeLists.txt
    ├── recon-unit/             # Reconnaissance processor
    │   ├── main.cpp
    │   └── CMakeLists.txt
    ├── logistics-depot/        # Supply management
    │   ├── main.cpp
    │   └── CMakeLists.txt
    └── tactical-display/       # Central monitoring
        ├── main.cpp
        └── CMakeLists.txt
```

## How Tracing Works

### 1. Trace Context in DDS Messages

Each DDS message includes a `TraceContext` struct in its header:

```idl
struct TraceContext {
    string trace_id;        // 32 hex characters (128-bit)
    string span_id;         // 16 hex characters (64-bit)
    string parent_span_id;  // Parent span ID
    octet trace_flags;      // Sampling flag (01 = sampled)
};

struct MissionOrder {
    TraceContext trace_ctx;  // Embedded trace context
    // ... payload fields
};
```

### 2. Root Span Creation (command-center)

The command-center creates root spans and embeds trace context into outgoing messages:

```cpp
// Start a new trace
auto span = tracer->StartSpan("issue-mission");

// Extract trace context
auto span_context = span->GetContext();
char trace_id_hex[33] = {0};
char span_id_hex[17] = {0};
span_context.trace_id().ToLowerBase16({trace_id_hex, 32});
span_context.span_id().ToLowerBase16({span_id_hex, 16});

// Embed in DDS message
msg.trace_ctx.trace_id = trace_id_hex;
msg.trace_ctx.span_id = span_id_hex;
```

### 3. Context Propagation (downstream services)

Downstream services extract parent context and create child spans:

```cpp
// Extract trace context from incoming DDS message
std::string trace_id_str = order->trace_ctx.trace_id;
std::string parent_span_str = order->trace_ctx.span_id;

// Parse hex strings to TraceId/SpanId
trace_api::TraceId trace_id;
uint8_t trace_id_buf[16];
for (int j = 0; j < 16; j++) {
    sscanf(trace_id_str.c_str() + j*2, "%2hhx", &trace_id_buf[j]);
}
trace_id = trace_api::TraceId(trace_id_buf);

// Create parent span context
auto parent_ctx = trace_api::SpanContext(trace_id, parent_span_id,
    trace_api::TraceFlags(trace_api::TraceFlags::kIsSampled), true);

// Start child span with parent context
trace_api::StartSpanOptions opts;
opts.parent = parent_ctx;
auto span = tracer->StartSpan("execute-recon", opts);
```

### 4. Trace Flow

```
command-center                recon-unit               logistics-depot          tactical-display
     │                             │                          │                        │
     │ issue-mission (root)        │                          │                        │
     │ trace_id: abc123...         │                          │                        │
     │ span_id: def456...          │                          │                        │
     ├────────────────────────────►│                          │                        │
     │                             │ execute-recon (child)    │                        │
     │                             │ trace_id: abc123...      │                        │
     │                             │ parent: def456...        │                        │
     │                             │ span_id: ghi789...       │                        │
     │                             ├─────────────────────────►│                        │
     │                             │                          │ dispatch-supplies      │
     │                             │                          │ trace_id: abc123...    │
     │                             │                          │ parent: ghi789...      │
     │                             │                          ├───────────────────────►│
     │                             │                          │                        │ display-*
     │                             │                          │                        │ trace_id: abc123...
```

All spans share the same `trace_id`, enabling end-to-end visibility in Jaeger.

## Quick Start

### Prerequisites

- Docker & Docker Compose
- Port 16686 available (Jaeger UI)

### Run

```bash
# Build and start all services
docker compose up --build

# Or run in background
docker compose up -d --build

# View logs
docker compose logs -f
```

### View Traces

1. Open Jaeger UI: **http://localhost:16686**
2. Select a service from the dropdown (e.g., `command-center`)
3. Click **Find Traces**
4. Click on a trace to see the full span tree across all services

## Example Output

```
[ORDER] RECON | Zone: Alpha | Priority: HIGH | ID: MSN-1734112800-0 | trace: 7c469062
[RECON] Mission: RECON | Zone: Alpha | Priority: HIGH | trace: 7c469062
[INTEL] TARGET CONFIRMED | Enemies: 25 | Threat: HIGH | Terrain: URBAN | trace: 7c469062
[DISPATCH] AMMO x30 -> Mission MSN-1734112800-0 | Stock: 70 | trace: 7c469062
[DISPLAY] NEW MISSION: RECON | Zone: Alpha | Priority: HIGH | trace: 7c469062
```

Notice how all log lines share the same trace prefix (`7c469062`), indicating they belong to the same distributed trace.

## Technology Stack

- **DDS**: Eclipse CycloneDDS
- **Tracing**: OpenTelemetry C++ SDK
- **Trace Backend**: Jaeger (OTLP HTTP)
- **Language**: C++17
- **Build**: CMake, Docker multi-stage builds

## Configuration

### CycloneDDS (`shared/cyclonedds.xml`)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<CycloneDDS>
  <Domain id="any">
    <General>
      <NetworkInterfaceAddress>auto</NetworkInterfaceAddress>
    </General>
  </Domain>
</CycloneDDS>
```

### OpenTelemetry Exporter

Each service exports traces via OTLP HTTP to Jaeger:

```cpp
otlp::OtlpHttpExporterOptions opts;
opts.url = "http://localhost:4318/v1/traces";
```

## Cleanup

```bash
docker compose down
docker compose down --volumes --rmi all  # Full cleanup
```