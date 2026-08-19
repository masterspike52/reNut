#pragma once

#include <cstdint>

namespace renut::game_activity_stats {

struct Snapshot {
    uint64_t animation_stream_frame = 0;
    uint64_t animation_stream_session = 0;
    uint64_t animation_stream_frame_ns = 0;
    uint64_t body_blend_frame = 0;
    uint64_t body_blend_session = 0;
    uint64_t body_blend_frame_ns = 0;
    uint64_t actor_script_frame = 0;
    uint64_t actor_script_session = 0;
    uint64_t actor_script_frame_ns = 0;
    uint64_t actor_generation_frame = 0;
    uint64_t actor_generation_session = 0;
    uint64_t actor_generation_frame_ns = 0;
};

void SetEnabled(bool enabled);
void RecordAnimationStream();
void FinishAnimationStream();
void RecordBodyBlend();
void FinishBodyBlend();
void RecordActorScript();
void FinishActorScript();
void RecordActorGeneration();
void FinishActorGeneration();
void EndFrame();
Snapshot GetSnapshot();

}
