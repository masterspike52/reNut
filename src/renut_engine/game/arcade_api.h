#pragma once

namespace renut::game::arcade_api {

// Before rexfabric::Start: declares renut.arcade, Klungo's arcade game in Showdown Town (its levels, sprites,
// tuning, Klungo and its enemies) and its events.
void Declare();

// renut.arcade.full_speed: a mod running its own game on the arcade's screen lets the frame limiter go past 30 fps.
bool FullSpeed();

} // namespace renut::game::arcade_api
