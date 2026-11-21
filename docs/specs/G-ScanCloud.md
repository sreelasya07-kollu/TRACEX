# Project G: ScanCloud
*Part G of the Room Scanner Program · Platform: accounts, storage, GPU jobs, monitoring · Prepared November 12, 2026*

## 1. Pitch
A multi-tenant platform for running GPU vision pipelines as reliable jobs: accounts, uploads, a job state machine with retries and fallbacks, GPU autoscaling, asset storage, notifications, billing, retention and observability. It also ships a gateway that keeps language-model and speech keys off phones. It runs the room scanner and can run any other heavy media pipeline.

## 2. Why it stands alone
GPU pipelines have the same operational problems everywhere: long jobs, expensive machines, partial failures, retries, cost surprises, and user data that must be deleted on request. ScanCloud packages the answers once. The source plan treats this as standard backend work (medium difficulty) that is easy to underestimate, so it is its own project with its own owner. Vid2scene is the architecture reference (queue, GPU worker, viewer), for study only.

## 3. Users and customers
- **Internal:** every other project runs on it.
- **External:** teams with their own pipelines (3D, video, audio) who want a hosted runner; developers who want an LLM and TTS gateway for mobile apps.

## 4. Scope
**In for v1:** authentication; scan service; presigned resumable uploads; job orchestrator with retries; one GPU worker pool; asset storage with signed URLs; push notifications; logs, metrics and per-job cost.

**Later:** autoscaling across GPU sizes, billing, retention automation, admin console, public API, multi-region.

## 5. Components
| Service | Job | Tag |
|---|---|---|
| API Gateway and Auth | Login, tokens, rate limits, routing | Buy and Integrate |
| Scan Service | Creates sessions, stores manifests, tracks status, lists scans | Build |
| Upload Service | Presigned URLs, chunk assembly, checksum checks | Integrate |
| Job Orchestrator | Runs pipeline stages with retries, timeouts, fallbacks and cancellation | Integrate (Temporal or Celery with Redis) |
| GPU Worker Pool and Autoscaler | Starts and stops GPU machines against queue depth; picks GPU size per job | Integrate and Buy |
| Asset Service | Stores results, versions them, serves signed URLs through a CDN | Buy |
| Dialog Proxy | Keeps LLM and TTS keys off the phone, rate limits, versions prompts, redacts logs | Build |
| Notification Service | Push and webhook events (scan ready, rescan needed) | Integrate |
| Billing and Entitlements | Plans, credits, per-job cost caps | Buy |
| Retention and Deletion Service | Enforces retention, handles user deletion and data export | Build |
| Analytics Pipeline | Product and quality events | Buy |
| Admin and Ops Console | Inspect failed jobs, replay a scan, view reports, manage users | Build |
| Observability Stack | Logs, metrics, traces, alerts, per-job cost | Integrate |

## 6. Key design decisions
- **Stages are containers with schemas.** The orchestrator knows stage names, inputs and outputs, not their internals, so Projects C and D can change freely.
- **Every job has a cost cap and a trace.** A scan ID and job ID travel through every module.
- **Idempotent stages and resumable uploads.** A retry never corrupts a result.
- **Remote config for thresholds, prompts and presets,** so tuning needs no app release.
- **Staging and production from day one,** with infrastructure as code and secrets management.

## 7. Spin-off products
1. **GPU Pipeline Runner (open source):** containerised stage workflows with retries, timeouts and per-job cost caps.
2. **Cost-Aware GPU Autoscaler:** picks the cheapest GPU that meets a job’s deadline and memory needs.
3. **LLM and TTS Gateway for Mobile Apps:** the Dialog Proxy as a product, with rate limits, prompt versioning and log redaction.
4. **Remote Config and Feature Flags Service** tuned for thresholds and prompt templates.
5. **Pipeline Replay and Debug Console:** rerun any stage from stored inputs, compare outputs, inspect timelines.
6. **3D-as-a-Service API:** third parties submit videos or bundles and pay per job.
7. **FinOps Dashboard for GPU Jobs:** cost per scan, per stage, per customer, with alerts.
8. **Webhook and Push Relay** for long-running jobs.
9. **Retention and Deletion Toolkit** shared with Project I.
10. **Serverless GPU Adapter:** one interface over several GPU providers, so jobs can move to the cheapest available capacity.

## 8. Achievements to aim for (verify prior art before claiming a first)
- Cost-capped jobs that stop or downgrade gracefully, with published cost-per-quality curves.
- Stage-level replay from stored inputs for every job, making any bad result reproducible.
- A gateway that keeps provider keys off devices while preserving urgent-line latency (templates stay local; see Project B).
- Autoscaling that meets a stated time-to-result promise at the lowest cost.
- Deletion that can be verified end to end (with Project I).

## 9. Roadmap
| Stage | Deliverable | Exit criterion (proposed) |
|---|---|---|
| G0 Minimal path | Object storage, one queue, one GPU worker, a basic API | A bundle becomes a model without manual steps |
| G1 State machine | Retries, timeouts, cancellation, status events | Injected failures recover or fail cleanly |
| G2 Resumable uploads and notifications | Presigned chunks, push events | Uploads survive network changes (with Project A) |
| G3 Auth and entitlements | Accounts, per-user isolation, cost caps | No cross-user access in tests |
| G4 Autoscaling | Scale GPU machines to queue depth | Meets time-to-result target at bounded cost |
| G5 Admin console and replay | Failed-job tools | A bad scan is diagnosed in minutes |
| G6 Billing and retention | Plans, credits, deletion and export | Deletion requests complete on schedule |
| G7 Public API and multi-region | Third-party access | An outside team runs a job from the docs |

## 10. Metrics
Job success rate; time to result; queue wait; GPU utilisation; cost per job and per stage; retry rate; upload completion rate; notification delivery; deletion completion time; availability.

## 11. Risks and mitigations
- **GPU cost overruns:** per-job caps, express mode (Project C), alerts.
- **Stuck or looping jobs:** timeouts, idempotence, dead-letter queues.
- **Vendor lock-in:** containers, infrastructure as code, provider adapter.
- **Sensitive home data:** isolation, encryption, retention rules (Project I).
- **Underestimated operations work:** treat it as a project with an owner from the start.

## 12. Interfaces with other projects
**In:** Scan Bundle (A); stage containers and manifests (C, D, E); asset manifest (F); consent and redaction manifests (I). **Out:** job and stage manifest contract; status events to the app; cost and performance data to H; remote config to A and B.

## 13. Team and first 30 days
One backend and infrastructure engineer, with a second as load grows. Week 1: stand up storage, a queue and a staging environment. Week 2: run Project C’s one-worker pipeline as a job. Week 3: add retries, cost logging and status events. Week 4: inject failures and write the first runbook.
