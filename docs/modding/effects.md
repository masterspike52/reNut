# Game effects for part scripts

Every particle effect in the game, as a part script plays it with `fx.play(effect, position)`:
pass the **ID** (as text, e.g. `"0x45F71639"`). Listed from the game's bundle files (704 effects).

**Where it works.** An effect can only play where the game has it loaded. Effects marked
*anywhere (common)* (154 of them) are in the bundle the game always keeps loaded; the others only
in the worlds listed (or the unidentified bundles, likely acts and challenges). If an effect isn't
loaded, `fx.play` skips it and logs a warning; nothing breaks.

**Finding one by sight.** Turn on `renut_log_effects` (F4, Debug) and do what shows the effect in
the game: the log prints each effect's ID the first time it plays.

Types: *GPU particle* and *3D GPU particle* are particle systems, *composite* combines several,
*emitter*/*particle* are older-style emitters, *blobs droplet* and *explosion* are special effects.

## Contents

- [actor](#actor) (109)
- [collectable](#collectable) (4)
- [common](#common) (204)
- [garage](#garage) (5)
- [level](#level) (44)
- [misc](#misc) (19)
- [particle](#particle) (4)
- [surfaceimpacts](#surfaceimpacts) (31)
- [vehicle](#vehicle) (20)
- [vehicleblocks](#vehicleblocks) (80)
- [vehicleparts](#vehicleparts) (7)
- [water](#water) (12)
- [weapon](#weapon) (80)
- [other](#other) (85)

## actor

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x45C2452E` | GPU particle | actor_banjo_bigsplashspray1 | anywhere (common) |
| `0x45DD37A2` | GPU particle | actor_banjo_bodyimpactdust | anywhere (common) |
| `0x4B8859B4` | composite | actor_banjo_bodyimpactfabric | anywhere (common) |
| `0x452B299E` | GPU particle | actor_banjo_bodyimpactless | anywhere (common) |
| `0x45BABFCF` | GPU particle | actor_banjo_bodyimpactsnowbits | anywhere (common) |
| `0x45C8B963` | GPU particle | actor_banjo_bodyimpactsnowdust | anywhere (common) |
| `0x1B2F3C1A` | emitter | actor_banjo_breathbubbles | anywhere (common) |
| `0x1C2F3C1A` | particle | actor_banjo_breathbubbles | anywhere (common) |
| `0x455931E8` | GPU particle | actor_banjo_footstepdust | anywhere (common) |
| `0x4542F965` | GPU particle | actor_banjo_footstepdustlight | anywhere (common) |
| `0x4B9A1EB8` | composite | actor_banjo_footstepsnow | anywhere (common) |
| `0x45175C45` | GPU particle | actor_banjo_footstepsnowbits | anywhere (common) |
| `0x45655AE9` | GPU particle | actor_banjo_footstepsnowdust | anywhere (common) |
| `0x45A6C7D9` | GPU particle | actor_banjo_footstepsplash | anywhere (common) |
| `0x4B32520C` | composite | actor_banjo_jumpfabric | anywhere (common) |
| `0x45F71639` | GPU particle | actor_banjo_jumpland | anywhere (common) |
| `0x453F2E44` | GPU particle | actor_banjo_jumplandless | anywhere (common) |
| `0x4B0A1F28` | composite | actor_banjo_jumplandsnow | anywhere (common) |
| `0x456E3161` | GPU particle | actor_banjo_jumplandsnowbits | anywhere (common) |
| `0x451C37CD` | GPU particle | actor_banjo_jumplandsnowdust | anywhere (common) |
| `0x45BD3103` | GPU particle | actor_banjo_splashoutspray | anywhere (common) |
| `0x1BCA296B` | emitter | actor_banjo_swimkickdroplets | anywhere (common) |
| `0x1CCA296B` | particle | actor_banjo_swimkickdroplets | anywhere (common) |
| `0x45751A48` | GPU particle | actor_banjo_swimkicksplash | anywhere (common) |
| `0x4509453D` | GPU particle | actor_banjo_wrenchhitdust | anywhere (common) |
| `0x45F78975` | GPU particle | actor_banjo_wrenchimpactsnowdust | anywhere (common) |
| `0x4A75F58E` | 3D GPU particle | actor_banjo_wrenchsparks1 | anywhere (common) |
| `0x1BFDB759` | emitter | actor_bubbles | anywhere (common) |
| `0x1CFDB759` | particle | actor_bubbles | anywhere (common) |
| `0x45A3056A` | GPU particle | actor_germdivideblobs | bundle 49e068 |
| `0x451EC190` | GPU particle | actor_germdivideblobsmedium | bundle 49e068 |
| `0x45690DF5` | GPU particle | actor_germdivideblobssmall | bundle 49e068 |
| `0x1B6CF19F` | emitter | actor_gruntbot_bolts | Banjoland, bundle bff707 |
| `0x1C6CF19F` | particle | actor_gruntbot_bolts | Banjoland, bundle bff707 |
| `0x1B6441F8` | emitter | actor_gruntbot_boltslarge | bundle bff707 |
| `0x1C6441F8` | particle | actor_gruntbot_boltslarge | bundle bff707 |
| `0x1BDB41F7` | emitter | actor_gruntbot_boltsmedium | bundle bff707 |
| `0x1CDB41F7` | particle | actor_gruntbot_boltsmedium | bundle bff707 |
| `0x1BF96F87` | emitter | actor_gruntbot_nuts | Banjoland, bundle bff707 |
| `0x1CF96F87` | particle | actor_gruntbot_nuts | Banjoland, bundle bff707 |
| `0x1B2BD83D` | emitter | actor_gruntbot_nutslarge | bundle bff707 |
| `0x1C2BD83D` | particle | actor_gruntbot_nutslarge | bundle bff707 |
| `0x1B013851` | emitter | actor_gruntbot_nutsmedium | bundle bff707 |
| `0x1C013851` | particle | actor_gruntbot_nutsmedium | bundle bff707 |
| `0x1B15BB21` | emitter | actor_gruntbot_spring | Banjoland, bundle bff707 |
| `0x1C15BB21` | particle | actor_gruntbot_spring | Banjoland, bundle bff707 |
| `0x1BBAC5DB` | emitter | actor_gruntbot_springlarge | bundle bff707 |
| `0x1CBAC5DB` | particle | actor_gruntbot_springlarge | bundle bff707 |
| `0x1BBAEE01` | emitter | actor_gruntbot_springmedium | bundle bff707 |
| `0x1CBAEE01` | particle | actor_gruntbot_springmedium | bundle bff707 |
| `0x1BA4AEDC` | emitter | actor_gruntbot_vehiclemetalexplode | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x1CA4AEDC` | particle | actor_gruntbot_vehiclemetalexplode | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x1B6F3A6B` | emitter | actor_gruntbot_vehiclemetalexplode1 | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x1C6F3A6B` | particle | actor_gruntbot_vehiclemetalexplode1 | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x455742A6` | GPU particle | actor_gruntbots_boatchimneysmoke | bundle 959f49, bundle e66c1c |
| `0x4590EAF2` | GPU particle | actor_gruntbots_boatwaterspray | bundle 959f49, bundle e66c1c |
| `0x45F569D4` | GPU particle | actor_gruntbots_buggyexhaustsmoke | bundle e1a271, bundle e66c1c |
| `0x45D7C893` | GPU particle | actor_gruntbots_buggyexplosionflash | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x455F7EEA` | GPU particle | actor_gruntbots_buggyexplosionsmoke | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x45DFE5EF` | GPU particle | actor_gruntbots_buggyexplosionsparks | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x45FAE850` | GPU particle | actor_gruntbots_charger_jump | Banjo's house, Banjoland, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 10ef2f, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 4db709, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle ee8a91, bundle fd97da |
| `0x45B8267B` | GPU particle | actor_gruntbots_charger_launchsparks | Banjo's house, bundle 01ede7, bundle 0b6509, bundle 1533cd, bundle 38aba7, bundle 58d076, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9f17a7, bundle b349d9, bundle b67961, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e66c1c, bundle eaaac4 |
| `0x453F25F0` | GPU particle | actor_gruntbots_deathsmoketrail | Banjo's house, Banjoland, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 10ef2f, bundle 1533cd, bundle 3112a5, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 4db709, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle ee8a91, bundle fd97da |
| `0x45B723DB` | GPU particle | actor_gruntbots_elecbeams | Banjo's house, Banjoland, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 10ef2f, bundle 1533cd, bundle 3112a5, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 4db709, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle ee8a91, bundle fd97da |
| `0x458B488A` | GPU particle | actor_gruntbots_elecbeamsexplode | Banjo's house, Banjoland, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 10ef2f, bundle 1533cd, bundle 3112a5, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 4db709, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle ee8a91, bundle fd97da |
| `0x453FE14C` | GPU particle | actor_gruntbots_elecbeamsexplode1 | Banjo's house, Banjoland, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 10ef2f, bundle 1533cd, bundle 3112a5, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 4db709, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle ee8a91, bundle fd97da |
| `0x45F713AA` | GPU particle | actor_gruntbots_exploder_sparks | bundle 100375, bundle 1533cd, bundle 49e068, bundle 7be8d2, bundle 959f49, bundle b67961, bundle c94cec, bundle cc7c54, bundle e66c1c |
| `0x455802B2` | GPU particle | actor_gruntbots_exploder_trail | bundle 100375, bundle 1533cd, bundle 49e068, bundle 7be8d2, bundle 959f49, bundle b67961, bundle c94cec, bundle cc7c54, bundle e66c1c |
| `0x45DFCBCF` | GPU particle | actor_gruntbots_explosionflash | Banjo's house, Banjoland, World of Sports, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 10ef2f, bundle 1533cd, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 49e068, bundle 4cff52, bundle 4db709, bundle 68f1b8, bundle 8c1797, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle cd43ee, bundle dd92c6, bundle e1a271, bundle e37086, bundle ee8a91, bundle fd97da |
| `0x45819BC1` | GPU particle | actor_gruntbots_explosionring | Banjo's house, Banjoland, Terrarium of Terror, World of Sports, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 47e9f3, bundle 4cff52, bundle 4db709, bundle 68f1b8, bundle 8c1797, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle cd43ee, bundle e37086, bundle ea08c0, bundle ea6e2d, bundle ee8a91, bundle fd97da |
| `0x45577DB6` | GPU particle | actor_gruntbots_explosionsmoke | Banjo's house, Banjoland, Terrarium of Terror, World of Sports, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 47e9f3, bundle 4cff52, bundle 4db709, bundle 68f1b8, bundle 8c1797, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle cd43ee, bundle e37086, bundle ea08c0, bundle ea6e2d, bundle ee8a91, bundle fd97da |
| `0x451BF033` | GPU particle | actor_gruntbots_explosionsparks | Banjo's house, Banjoland, Terrarium of Terror, World of Sports, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 47e9f3, bundle 4cff52, bundle 4db709, bundle 68f1b8, bundle 8c1797, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle cd43ee, bundle e37086, bundle ea08c0, bundle ea6e2d, bundle ee8a91, bundle fd97da |
| `0x459D84C4` | GPU particle | actor_gruntbots_flytrail | Banjo's house, Banjoland, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 10ef2f, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 4db709, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle ee8a91, bundle fd97da |
| `0x45B4E80E` | GPU particle | actor_gruntbots_large_deathsmoketrail | Banjo's house, bundle 01ede7, bundle 100375, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 9a271f, bundle e1a271 |
| `0x459FC563` | GPU particle | actor_gruntbots_large_elecbeams | Banjo's house, bundle 01ede7, bundle 100375, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 9a271f, bundle e1a271 |
| `0x45491C5C` | GPU particle | actor_gruntbots_large_elecbeamsexplode | Banjo's house, bundle 01ede7, bundle 100375, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 9a271f, bundle e1a271 |
| `0x45D554F9` | GPU particle | actor_gruntbots_large_elecbeamsexplode1 | Banjo's house, bundle 01ede7, bundle 100375, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 9a271f, bundle e1a271 |
| `0x45BA81B4` | GPU particle | actor_gruntbots_large_explosionflash | Banjo's house, bundle 01ede7, bundle 100375, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 9a271f, bundle e1a271 |
| `0x453237CD` | GPU particle | actor_gruntbots_large_explosionsmoke | Banjo's house, bundle 01ede7, bundle 100375, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 9a271f, bundle e1a271 |
| `0x45903DCD` | GPU particle | actor_gruntbots_large_explosionsparks | Banjo's house, bundle 01ede7, bundle 100375, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 9a271f, bundle e1a271 |
| `0x4530CA27` | GPU particle | actor_gruntbots_medium_deathsmoketrail | Banjo's house, bundle 01ede7, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle eaaac4, bundle ef9a7c |
| `0x4531CBFA` | GPU particle | actor_gruntbots_medium_elecbeams | Banjo's house, bundle 01ede7, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle eaaac4, bundle ef9a7c |
| `0x45CA0012` | GPU particle | actor_gruntbots_medium_elecbeamsexplode | Banjo's house, bundle 01ede7, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle eaaac4, bundle ef9a7c |
| `0x45F3BB72` | GPU particle | actor_gruntbots_medium_elecbeamsexplode1 | Banjo's house, bundle 01ede7, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle eaaac4, bundle ef9a7c |
| `0x4516471A` | GPU particle | actor_gruntbots_medium_explosionflash | Banjo's house, bundle 01ede7, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle eaaac4, bundle ef9a7c |
| `0x459EF163` | GPU particle | actor_gruntbots_medium_explosionsmoke | Banjo's house, bundle 01ede7, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle eaaac4, bundle ef9a7c |
| `0x45141FE4` | GPU particle | actor_gruntbots_medium_explosionsparks | Banjo's house, bundle 01ede7, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle eaaac4, bundle ef9a7c |
| `0x451CFBAC` | GPU particle | actor_gruntbots_swimtrail | Banjo's house, Banjoland, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 10ef2f, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 4db709, bundle 68f1b8, bundle 6dc100, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle ee8a91, bundle fd97da |
| `0x4B7E8532` | composite | actor_gruntbotvehicleexplode | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x453D2539` | GPU particle | actor_jinjo_disappear | Banjo's house, Mumbo's Motors, Showdown Town, bundle 01ca49, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 7be8d2, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle e66c1c, bundle eaaac4, bundle ef9a7c |
| `0x4B3D2539` | composite | actor_jinjo_disappear | Banjo's house, Mumbo's Motors, Showdown Town, bundle 01ca49, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 7be8d2, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle e66c1c, bundle eaaac4, bundle ef9a7c |
| `0x45E6B517` | GPU particle | actor_jinjo_disappear1 | Banjo's house, Mumbo's Motors, Showdown Town, bundle 01ca49, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 7be8d2, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle e66c1c, bundle eaaac4, bundle ef9a7c |
| `0x456A1398` | GPU particle | actor_jinjo_disappearflash | Banjo's house, Mumbo's Motors, Showdown Town, bundle 01ca49, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 7be8d2, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle e66c1c, bundle eaaac4, bundle ef9a7c |
| `0x4BFC7860` | composite | actor_jinjo_spiral | Banjo's house, Mumbo's Motors, Showdown Town, bundle 01ca49, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 7be8d2, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle e66c1c, bundle eaaac4, bundle ef9a7c |
| `0x45E612E0` | GPU particle | actor_jinjo_spiraltrail | Banjo's house, Mumbo's Motors, Showdown Town, bundle 01ca49, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 7be8d2, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle e66c1c, bundle eaaac4, bundle ef9a7c |
| `0x45420450` | GPU particle | actor_jinjo_spiraltrail1 | Banjo's house, Mumbo's Motors, Showdown Town, bundle 01ca49, bundle 01ede7, bundle 04dd5f, bundle 0b6509, bundle 100375, bundle 1533cd, bundle 3112a5, bundle 34221d, bundle 34648e, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 68f1b8, bundle 6dc100, bundle 7be8d2, bundle 90aff1, bundle 959f49, bundle 9a271f, bundle 9f17a7, bundle a5600b, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle cc7c54, bundle dd92c6, bundle e1a271, bundle e66c1c, bundle eaaac4, bundle ef9a7c |
| `0x4506FEB5` | GPU particle | actor_police_hovercraftsmoke | Showdown Town, bundle 959f49, bundle e1a271, bundle eaaac4 |
| `0x1BF7A9A4` | emitter | actor_slide | anywhere (common) |
| `0x1CF7A9A4` | particle | actor_slide | anywhere (common) |
| `0x1B421271` | emitter | actor_watersplashdroplets | anywhere (common) |
| `0x1C421271` | particle | actor_watersplashdroplets | anywhere (common) |
| `0x1BFA4B7C` | emitter | actor_watersplashdropletsbackin | anywhere (common) |
| `0x1CFA4B7C` | particle | actor_watersplashdropletsbackin | anywhere (common) |
| `0x1BA0B2E6` | emitter | actor_watersplashfootstep | anywhere (common) |
| `0x1CA0B2E6` | particle | actor_watersplashfootstep | anywhere (common) |
| `0x1BF84FCB` | emitter | actor_watersplashin | anywhere (common) |
| `0x1CF84FCB` | particle | actor_watersplashin | anywhere (common) |
| `0x1B8804B5` | emitter | actor_waterswimkick | anywhere (common) |
| `0x1C8804B5` | particle | actor_waterswimkick | anywhere (common) |

## collectable

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x1BBEB19C` | emitter | collectable_notepickup | anywhere (common) |
| `0x1CBEB19C` | particle | collectable_notepickup | anywhere (common) |
| `0x1B018845` | emitter | collectable_notetwinkle | Banjo's house |
| `0x1C018845` | particle | collectable_notetwinkle | Banjo's house |

## common

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x452C4936` | GPU particle | common_banjowatershake1 | anywhere (common) |
| `0x4525188C` | GPU particle | common_banjowatershake2 | anywhere (common) |
| `0x4509DAED` | GPU particle | common_bbqfire1 | Banjo's house, Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x45008B57` | GPU particle | common_bbqfire2 | Banjo's house, Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x453B98D5` | GPU particle | common_bbqsmoke1 | Banjo's house, Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x4532C96F` | GPU particle | common_bbqsmoke2 | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x45159AA1` | GPU particle | common_bbqsmokefaraway1 | Nutty Acres |
| `0x459F5B38` | GPU particle | common_beancandust1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle e66c1c, bundle ee8a91, bundle fd97da |
| `0x45960A82` | GPU particle | common_beancandust2 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle e66c1c, bundle ee8a91, bundle fd97da |
| `0x4592AED2` | GPU particle | common_bombdust1 | bundle 04dd5f, bundle 90aff1, bundle e66c1c, bundle eaaac4 |
| `0x459BFF68` | GPU particle | common_bombdust2 | bundle 04dd5f, bundle 90aff1, bundle e66c1c, bundle eaaac4 |
| `0x4532CCC9` | GPU particle | common_bpsteamshoot1 | Banjoland, bundle 0b6509, bundle 10ef2f, bundle 38aba7, bundle 4db709, bundle 9a271f, bundle 9f17a7, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle ee8a91, bundle fd97da |
| `0x45B08BF2` | GPU particle | common_bpsteamslow1 | Banjoland, bundle 0b6509, bundle 10ef2f, bundle 38aba7, bundle 4db709, bundle 9a271f, bundle 9f17a7, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle ee8a91, bundle fd97da |
| `0x45EB434E` | GPU particle | common_bugzapper1 | anywhere (common) |
| `0x45E212F4` | GPU particle | common_bugzapper2 | anywhere (common) |
| `0x45E52262` | GPU particle | common_bugzapper3 | anywhere (common) |
| `0x4581B7C1` | GPU particle | common_bugzapper4 | anywhere (common) |
| `0x45868757` | GPU particle | common_bugzapper5 | anywhere (common) |
| `0x4560D005` | GPU particle | common_chimneysmoke | Spiral Mountain, Spiral Mountain / title screen |
| `0x45D4C0E5` | GPU particle | common_cpuwind1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x45DD915F` | GPU particle | common_cpuwind2 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x45AC04E5` | GPU particle | common_cratemagic1a | bundle e66c1c |
| `0x45A5555F` | GPU particle | common_cratemagic1b | bundle e66c1c |
| `0x45A265C9` | GPU particle | common_cratemagic1c | bundle e66c1c |
| `0x451B433E` | GPU particle | common_cratepulse1a | bundle e66c1c |
| `0x45121284` | GPU particle | common_cratepulse1b | bundle e66c1c |
| `0x45915E86` | GPU particle | common_deedsmagic1 | streamed only |
| `0x45980F3C` | GPU particle | common_deedsmagic2 | streamed only |
| `0x459F3FAA` | GPU particle | common_deedsmagic3 | streamed only |
| `0x45FBAA09` | GPU particle | common_deedsmagic4 | streamed only |
| `0x457F95D2` | GPU particle | common_dicedestroy1 | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4576C468` | GPU particle | common_dicedestroy2 | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4571F4FE` | GPU particle | common_dicedestroy3 | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x452BA73A` | GPU particle | common_dicedestroytrail1 | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x45A42744` | GPU particle | common_digitalnumbers1 | LOGBOX 720 |
| `0x45AD76FE` | GPU particle | common_digitalnumbers2 | LOGBOX 720 |
| `0x45AA4668` | GPU particle | common_digitalnumbers3 | LOGBOX 720 |
| `0x45E4DADA` | GPU particle | common_digitalnumbersblue1 | LOGBOX 720 |
| `0x45ED8B60` | GPU particle | common_digitalnumbersblue2 | LOGBOX 720 |
| `0x45EABBF6` | GPU particle | common_digitalnumbersblue3 | LOGBOX 720 |
| `0x45EA8F15` | GPU particle | common_digitalnumberspink1 | LOGBOX 720 |
| `0x45E3DEAF` | GPU particle | common_digitalnumberspink2 | LOGBOX 720 |
| `0x45E4EE39` | GPU particle | common_digitalnumberspink3 | LOGBOX 720 |
| `0x457DFC8F` | GPU particle | common_digitalnumbersyellow1 | LOGBOX 720 |
| `0x4574AD35` | GPU particle | common_digitalnumbersyellow2 | LOGBOX 720 |
| `0x45739DA3` | GPU particle | common_digitalnumbersyellow3 | LOGBOX 720 |
| `0x455FE457` | GPU particle | common_eggexplode1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x4556B5ED` | GPU particle | common_eggexplode2 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x4551857B` | GPU particle | common_eggexplode3 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45197B4A` | GPU particle | common_eggexplodetrail1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x453865BC` | GPU particle | common_energyballedge1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba |
| `0x45313406` | GPU particle | common_energyballedge2 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba |
| `0x45FC2A8E` | GPU particle | common_energyballmain1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba |
| `0x457B4837` | GPU particle | common_fanoverheat1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x45C81614` | GPU particle | common_faultywires1 | Banjoland, Showdown Town, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45C147AE` | GPU particle | common_faultywires2 | Banjoland, Showdown Town, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45C67738` | GPU particle | common_faultywires3 | Banjoland, Showdown Town, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45A2E29B` | GPU particle | common_faultywires4 | Banjoland, Showdown Town, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45D87B48` | GPU particle | common_firewall1 | bundle e66c1c |
| `0x45D61A64` | GPU particle | common_firewall3 | bundle e66c1c |
| `0x455EB132` | GPU particle | common_flubberdrip1 | LOGBOX 720 |
| `0x4557E088` | GPU particle | common_flubberdrip2 | LOGBOX 720 |
| `0x4550D01E` | GPU particle | common_flubberdrip3 | LOGBOX 720 |
| `0x45834DA4` | GPU particle | common_flubbergush1 | LOGBOX 720 |
| `0x458A1C1E` | GPU particle | common_flubbergush2 | LOGBOX 720 |
| `0x458D2C88` | GPU particle | common_flubbergush3 | LOGBOX 720 |
| `0x459AEC7D` | GPU particle | common_flubbersplat1 | Mumbo's Motors, Showdown Town, bundle 01ede7, bundle 1533cd, bundle e66c1c |
| `0x45DB5601` | GPU particle | common_garagetorch1 | Mumbo's Motors |
| `0x45D207BB` | GPU particle | common_garagetorch2 | Mumbo's Motors |
| `0x45D5372D` | GPU particle | common_garagetorch3 | Mumbo's Motors |
| `0x456940F8` | GPU particle | common_germeffect1 | Terrarium of Terror, bundle 34648e, bundle 47e9f3, bundle ea08c0, bundle ea6e2d |
| `0x45601142` | GPU particle | common_germeffect2 | Terrarium of Terror, bundle 34648e, bundle 47e9f3, bundle ea08c0, bundle ea6e2d |
| `0x456721D4` | GPU particle | common_germeffect3 | Terrarium of Terror, bundle 34648e, bundle 47e9f3, bundle ea08c0, bundle ea6e2d |
| `0x4503B477` | GPU particle | common_germeffect4 | Terrarium of Terror, bundle 34648e, bundle 47e9f3, bundle ea08c0, bundle ea6e2d |
| `0x458CF272` | GPU particle | common_ggblast1 | Showdown Town |
| `0x45917E79` | GPU particle | common_ggconstantrings1 | Showdown Town |
| `0x45B58E96` | GPU particle | common_gginvis1 | Showdown Town |
| `0x457984E8` | GPU particle | common_ggtrail1 | Showdown Town |
| `0x45CB4FA1` | GPU particle | common_hoopboostjet1 | bundle 34a511, bundle 6dc100, bundle e66c1c |
| `0x45C21E1B` | GPU particle | common_hoopboostjet2 | bundle 34a511, bundle 6dc100, bundle e66c1c |
| `0x45C52E8D` | GPU particle | common_hoopboostjet3 | bundle 34a511, bundle 6dc100, bundle e66c1c |
| `0x456C0804` | GPU particle | common_hoophoverjet1 | bundle 34a511, bundle 6dc100, bundle e66c1c |
| `0x456559BE` | GPU particle | common_hoophoverjet2 | bundle 34a511, bundle 6dc100, bundle e66c1c |
| `0x45626928` | GPU particle | common_hoophoverjet3 | bundle 34a511, bundle 6dc100, bundle e66c1c |
| `0x45DA2338` | GPU particle | common_iceblockslide1 | bundle 7be8d2, bundle e66c1c |
| `0x45D37282` | GPU particle | common_iceblockslide2 | bundle 7be8d2, bundle e66c1c |
| `0x45458654` | GPU particle | common_icebreaker1 | Banjoland |
| `0x454CD7EE` | GPU particle | common_icebreaker2 | Banjoland |
| `0x454BE778` | GPU particle | common_icebreaker3 | Banjoland |
| `0x452F72DB` | GPU particle | common_icebreaker4 | Banjoland |
| `0x45B077C7` | GPU particle | common_icecubeexpl1 | bundle 7be8d2, bundle e66c1c |
| `0x45B9267D` | GPU particle | common_icecubeexpl2 | bundle 7be8d2 |
| `0x4521E2D1` | GPU particle | common_iceexplosiondebris1 | bundle 7be8d2 |
| `0x4528B36B` | GPU particle | common_iceexplosiondebris2 | bundle 7be8d2 |
| `0x452F83FD` | GPU particle | common_iceexplosiondebris3 | bundle 7be8d2 |
| `0x45812C39` | GPU particle | common_iceexplosiontrail1 | bundle 7be8d2 |
| `0x457229D9` | GPU particle | common_iceimpact1 | bundle 7be8d2 |
| `0x457B7863` | GPU particle | common_iceimpact2 | bundle 7be8d2 |
| `0x457C48F5` | GPU particle | common_iceimpact3 | bundle 7be8d2 |
| `0x4518DD56` | GPU particle | common_iceimpact4 | bundle 7be8d2 |
| `0x452C2E06` | GPU particle | common_iceslidemain1 | bundle 7be8d2, bundle e66c1c |
| `0x4578B60F` | GPU particle | common_jingotransform1 | anywhere (common) |
| `0x4571E7B5` | GPU particle | common_jingotransform2 | anywhere (common) |
| `0x4576D723` | GPU particle | common_jingotransform3 | anywhere (common) |
| `0x45124280` | GPU particle | common_jingotransform4 | anywhere (common) |
| `0x45056E58` | GPU particle | common_kazooiefeathers1 | anywhere (common) |
| `0x450C3FE2` | GPU particle | common_kazooiefeathers2 | anywhere (common) |
| `0x457BAE56` | GPU particle | common_leaffall1 | Banjoland |
| `0x4551AD69` | GPU particle | common_logappear1 | Spiral Mountain, Spiral Mountain / title screen |
| `0x4558FCD3` | GPU particle | common_logappear2 | Spiral Mountain, Spiral Mountain / title screen |
| `0x455FCC45` | GPU particle | common_logappear3 | Spiral Mountain, Spiral Mountain / title screen |
| `0x45A6C3F0` | GPU particle | common_logolympicsteam1 | bundle 4e967e, bundle e66c1c |
| `0x45AE1AD6` | GPU particle | common_logolympictorch1 | bundle 4e967e, bundle e66c1c |
| `0x45A74B6C` | GPU particle | common_logolympictorch2 | bundle 4e967e, bundle e66c1c |
| `0x45A07BFA` | GPU particle | common_logolympictorch3 | bundle 4e967e, bundle e66c1c |
| `0x45592CEA` | GPU particle | common_logspalaceopendoor4 | Showdown Town |
| `0x455DC0DF` | GPU particle | common_missilebeambolt1 | Banjoland |
| `0x4543EBED` | GPU particle | common_missilebeamtrail1 | Banjoland |
| `0x454ABA57` | GPU particle | common_missilebeamtrail2 | Banjoland |
| `0x45A1211B` | GPU particle | common_mrpatchdrip1 | bundle e66c1c, bundle eaaac4 |
| `0x45A870A1` | GPU particle | common_mrpatchdrip2 | bundle e66c1c, bundle eaaac4 |
| `0x45AF4037` | GPU particle | common_mrpatchdrip3 | bundle e66c1c, bundle eaaac4 |
| `0x45A09059` | GPU particle | common_mrpatchsplash1a | Banjoland |
| `0x45A9C1E3` | GPU particle | common_mrpatchsplash1b | Banjoland |
| `0x45AEF175` | GPU particle | common_mrpatchsplash1c | Banjoland |
| `0x45CA64D6` | GPU particle | common_mrpatchsplash1d | Banjoland |
| `0x45CD5440` | GPU particle | common_mrpatchsplash1e | Banjoland |
| `0x45C405FA` | GPU particle | common_mrpatchsplash1f | Banjoland |
| `0x451165CE` | GPU particle | common_mumbocrateexplode1 | bundle e66c1c |
| `0x45183474` | GPU particle | common_mumbocrateexplode2 | bundle e66c1c |
| `0x451F04E2` | GPU particle | common_mumbocrateexplode3 | bundle e66c1c |
| `0x457CA1D7` | GPU particle | common_mumbocrateexplode5 | bundle e66c1c |
| `0x4520EB43` | GPU particle | common_mumbocratetrail1 | bundle e66c1c |
| `0x450E30CD` | GPU particle | common_mumbomagichands1 | anywhere (common) |
| `0x45076177` | GPU particle | common_mumbomagichands2 | anywhere (common) |
| `0x450051E1` | GPU particle | common_mumbomagichands3 | anywhere (common) |
| `0x4564C442` | GPU particle | common_mumbomagichands4 | anywhere (common) |
| `0x4550B5AA` | GPU particle | common_mumboswitchactivate1 | Banjoland, Showdown Town, bundle 0b6509, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 4db709, bundle 8c1797, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle cd43ee, bundle e37086, bundle ee8a91, bundle fd97da |
| `0x4559E410` | GPU particle | common_mumboswitchactivate2 | Banjoland, Showdown Town, bundle 0b6509, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 4db709, bundle 8c1797, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle cd43ee, bundle e37086, bundle ee8a91, bundle fd97da |
| `0x455ED486` | GPU particle | common_mumboswitchactivate3 | Banjoland, Showdown Town, bundle 0b6509, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 4db709, bundle 8c1797, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle cd43ee, bundle e37086, bundle ee8a91, bundle fd97da |
| `0x453A4125` | GPU particle | common_mumboswitchactivate4 | Banjoland, Showdown Town, bundle 0b6509, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 4db709, bundle 8c1797, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle cd43ee, bundle e37086, bundle ee8a91, bundle fd97da |
| `0x45AF2BE4` | GPU particle | common_piddlestrail1 | Banjo's house, Mumbo's Motors, Showdown Town, bundle 100375, bundle 68f1b8, bundle 8c1797, bundle b67961, bundle c3c402, bundle c94cec, bundle cc7c54, bundle e66c1c |
| `0x4562E790` | GPU particle | common_pillevaporate1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba |
| `0x456BB62A` | GPU particle | common_pillevaporate2 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba |
| `0x45E29988` | GPU particle | common_pizzacrumbs1 | Mumbo's Motors, bundle 01ca49, bundle e66c1c |
| `0x45EBC832` | GPU particle | common_pizzacrumbs2 | Mumbo's Motors, bundle 01ca49, bundle e66c1c |
| `0x45ECF8A4` | GPU particle | common_pizzacrumbs3 | Mumbo's Motors, bundle 01ca49, bundle e66c1c |
| `0x45B6B491` | GPU particle | common_pizzadrip2 | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086, bundle e66c1c |
| `0x4546A5E9` | GPU particle | common_pizzasplat1 | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086, bundle e66c1c |
| `0x45E3DB27` | GPU particle | common_planetrail1 | bundle 959f49, bundle e66c1c, bundle eaaac4 |
| `0x45F63F34` | GPU particle | common_roaid_gpuparticleeffect_common_rocktrail3ckimpactdust2 | streamed only |
| `0x45CD5D73` | GPU particle | common_rockcoolsteam1 | bundle 90aff1, bundle e66c1c |
| `0x45763456` | GPU particle | common_rockfire1 | bundle 90aff1, bundle e66c1c |
| `0x457F65EC` | GPU particle | common_rockfire2 | bundle 90aff1, bundle e66c1c |
| `0x4578557A` | GPU particle | common_rockfire3 | bundle 90aff1, bundle e66c1c |
| `0x45FBBDA5` | GPU particle | common_rockimpactdust1 | Banjo's house, Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45F2EC1F` | GPU particle | common_rockimpactdust2 | Banjo's house, Banjoland, Mumbo's Motors, Showdown Town, Spiral Mountain, Spiral Mountain / title screen, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 4db709, bundle 8c1797, bundle 90aff1, bundle 9a271f, bundle 9cedca, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086, bundle e66c1c, bundle ee8a91, bundle fd97da |
| `0x45F5DC89` | GPU particle | common_rockimpactdust3 | Mumbo's Motors, bundle 01ca49, bundle 90aff1, bundle e66c1c |
| `0x4591492A` | GPU particle | common_rockimpactdust4 | anywhere (common) |
| `0x45A138AF` | GPU particle | common_rockpoisongas1 | bundle 90aff1, bundle e66c1c |
| `0x45A86915` | GPU particle | common_rockpoisongas2 | bundle 90aff1, bundle e66c1c |
| `0x456DA5C0` | GPU particle | common_rocktrail1 | Nutty Acres, bundle e66c1c |
| `0x455F0BA8` | GPU particle | common_rundust1 | anywhere (common) |
| `0x45565A12` | GPU particle | common_rundust2 | bundle e66c1c, bundle eaaac4 |
| `0x45AEDEAD` | GPU particle | common_smallbits1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45A78F17` | GPU particle | common_smallbits2 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x4518A975` | GPU particle | common_smashpipe1 | Banjo's house, bundle 0b6509, bundle 1533cd, bundle 38aba7, bundle 49e068, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle e66c1c |
| `0x4511F8CF` | GPU particle | common_smashpipe2 | Banjo's house, bundle 0b6509, bundle 1533cd, bundle 38aba7, bundle 49e068, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle e66c1c |
| `0x4586FA2D` | GPU particle | common_smoke_statuering1 | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086, bundle e66c1c |
| `0x450E0319` | GPU particle | common_smoke_waterhitspray1 | World of Sports |
| `0x4550AD12` | GPU particle | common_snowexpl1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45DD95C3` | GPU particle | common_snowfall1 | Banjoland |
| `0x45D4C479` | GPU particle | common_snowfall2 | Banjoland |
| `0x457981CB` | GPU particle | common_snowring1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45EE4DDA` | GPU particle | common_sparks_waterhose1 | World of Sports |
| `0x4534EEA3` | GPU particle | common_sprinklerbubbles1 | Terrarium of Terror, bundle 47e9f3, bundle ea08c0, bundle ea6e2d |
| `0x45475DBE` | GPU particle | common_steamshoot1 | Banjo's house, Spiral Mountain, Spiral Mountain / title screen, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086, bundle e66c1c |
| `0x45B18921` | GPU particle | common_steamshootbig1 | Banjoland |
| `0x454D518D` | GPU particle | common_steamslow1 | Spiral Mountain, Spiral Mountain / title screen, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086, bundle e66c1c |
| `0x45FC7479` | GPU particle | common_steamslowbig1 | Banjoland |
| `0x4507024E` | GPU particle | common_sweepdust1 | bundle e66c1c |
| `0x450E53F4` | GPU particle | common_sweepdust2 | bundle e66c1c |
| `0x455AD41B` | GPU particle | common_targetzandust1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45A1BAA6` | GPU particle | common_torchfirebig1 | World of Sports |
| `0x45A8EB1C` | GPU particle | common_torchfirebig2 | World of Sports |
| `0x45A075E9` | GPU particle | common_trail1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x457168B7` | GPU particle | common_trailbig1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x45216C49` | GPU particle | common_ttshing1 | bundle 3112a5, bundle 34221d, bundle 6dc100 |
| `0x45283DF3` | GPU particle | common_ttshing2 | bundle 3112a5, bundle 34221d, bundle 6dc100 |
| `0x452F0D65` | GPU particle | common_ttshing3 | bundle 3112a5, bundle 34221d, bundle 6dc100 |
| `0x458D13CF` | GPU particle | common_volcanoexplosion1 | Nutty Acres |
| `0x45844275` | GPU particle | common_volcanoexplosion2 | Nutty Acres |
| `0x458372E3` | GPU particle | common_volcanoexplosion3 | Nutty Acres |
| `0x45E5D26A` | GPU particle | common_waterdrip1 | Banjoland |
| `0x45EC83D0` | GPU particle | common_waterdrip2 | Banjoland |
| `0x45EBB346` | GPU particle | common_waterdrip3 | Banjoland |
| `0x45C58ED8` | GPU particle | common_watersplat1 | Banjoland |
| `0x4570F62A` | GPU particle | common_wrenchidle1 | anywhere (common) |
| `0x4579A790` | GPU particle | common_wrenchidle2 | anywhere (common) |
| `0x457E9706` | GPU particle | common_wrenchidle3 | anywhere (common) |
| `0x45CC35CF` | GPU particle | common_wrenchmagic1 | anywhere (common) |
| `0x45C56475` | GPU particle | common_wrenchmagic2 | anywhere (common) |
| `0x45C254E3` | GPU particle | common_wrenchmagic3 | anywhere (common) |
| `0x45A6C140` | GPU particle | common_wrenchmagic4 | anywhere (common) |

## garage

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x454A0884` | GPU particle | garage_blockappear | Mumbo's Motors |
| `0x458E93BE` | GPU particle | garage_blockdisappear | Mumbo's Motors |
| `0x4565E4C6` | GPU particle | garage_blueprintload | Mumbo's Motors |
| `0x45A3F45D` | GPU particle | garage_cursorappear | anywhere (common) |
| `0x45E626EB` | GPU particle | garage_tutorialmarkersparkles | Mumbo's Motors |

## level

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x457113C0` | GPU particle | level_cpu_fanunitglow1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x4578427A` | GPU particle | level_cpu_fanunitglow2 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x451BE74F` | GPU particle | level_cpu_fanunitglow4 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x45DC8D57` | GPU particle | level_cpu_fanunitsmoke | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x4589AB00` | GPU particle | level_cpu_fanunitspark | bundle 01ede7, bundle 0b6509, bundle 1533cd, bundle 38aba7, bundle 49e068, bundle 58d076, bundle 9a271f, bundle 9f17a7, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle e1a271, bundle e66c1c, bundle eaaac4 |
| `0x4521BBA4` | GPU particle | level_flare | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x4509BCAA` | GPU particle | level_flarecutscene | bundle 90aff1 |
| `0x451A2DF2` | GPU particle | level_greenrocksmoke | Terrarium of Terror, bundle 47e9f3, bundle ea08c0, bundle ea6e2d |
| `0x4B28CA84` | composite | level_jinjobridgefire | Banjoland |
| `0x45F0895E` | GPU particle | level_jinjobridgeflamebasesmall | Banjoland |
| `0x453D4DE3` | GPU particle | level_jinjobridgeflamebitssmall | Banjoland |
| `0x459FB3B5` | GPU particle | level_jinjobridgeflamesmall | Banjoland |
| `0x45D4E14D` | GPU particle | level_pipesmoke | Showdown Town |
| `0x45D323E8` | GPU particle | level_smokesignal | anywhere (common) |
| `0x45BE0583` | GPU particle | level_sumoringfire | bundle 181575 |
| `0x4BBE0583` | composite | level_sumoringfire | bundle 181575 |
| `0x450D523F` | GPU particle | level_sumoringfirestraightflame | bundle 181575 |
| `0x454BD3E6` | GPU particle | level_sumoringfirestraightflamebase | bundle 181575 |
| `0x453E11A1` | GPU particle | level_volcanesmoke3 | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x454A9012` | GPU particle | level_volcanopipefire1 | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x45ADB6FA` | GPU particle | level_volcanopipesparks1 | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x457FED90` | GPU particle | level_volcanosmokecheap1 | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x4597520A` | GPU particle | level_volcanosmokefrompipe | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x45EE64A3` | GPU particle | level_volcanosparks1 | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x45626352` | GPU particle | level_waterdrip | bundle 01ede7, bundle 04dd5f, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle 90aff1, bundle c94cec, bundle cc7c54 |
| `0x457B033B` | GPU particle | level_waterdrip2 | bundle 01ede7, bundle 04dd5f, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle 90aff1, bundle c94cec, bundle cc7c54 |
| `0x457C33AD` | GPU particle | level_waterdrip3 | bundle 01ede7, bundle 04dd5f, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle 90aff1, bundle c94cec, bundle cc7c54 |
| `0x456D0657` | GPU particle | level_waterfallbottommist | Nutty Acres |
| `0x45921095` | GPU particle | level_wosflamebitssmall | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x45E3A45B` | GPU particle | level_wosflamebitssmall2 | World of Sports, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 68f1b8, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4BBAA785` | composite | level_wostorchfire | World of Sports |
| `0x4B399E90` | composite | level_wostorchfiresmall | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4B84FB5A` | composite | level_wostorchfiresmall2 | World of Sports, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 68f1b8, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x457F22AA` | GPU particle | level_wostorchflamebits | World of Sports |
| `0x450DDDA8` | GPU particle | level_wostorchstraightflame | World of Sports |
| `0x456721E9` | GPU particle | level_wostorchstraightflamebase | World of Sports |
| `0x45383B90` | GPU particle | level_wostorchstraightflamebasesmall | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4508FAFF` | GPU particle | level_wostorchstraightflamebasesmall2 | World of Sports, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 68f1b8, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x45AFDD53` | GPU particle | level_wostorchstraightflamesmall | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x45F1FE13` | GPU particle | level_wostorchstraightflamesmall2 | World of Sports, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 68f1b8, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x45626920` | GPU particle | level_wosundergroundflamebits | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4523C287` | GPU particle | level_wosundergroundstraightflame | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x454CBA42` | GPU particle | level_wosundergroundstraightflamebase | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4B9BA226` | composite | level_wosundergroundtorchfire | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |

## misc

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x450AFECE` | GPU particle | misc_characterimpactdust | anywhere (common) |
| `0x45BB4FCD` | GPU particle | misc_cookingeggsmoke | anywhere (common) |
| `0x45EB52A2` | GPU particle | misc_explosion | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x45E71BB1` | GPU particle | misc_explosionsmoke | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x45B33787` | GPU particle | misc_ghostdisappear | Showdown Town |
| `0x4535AC9D` | GPU particle | misc_jiggybankdigitchangeeffect | Showdown Town |
| `0x45C55FC2` | GPU particle | misc_jiggybanksuck | Showdown Town |
| `0x4582A557` | GPU particle | misc_jiggysparkle | anywhere (common) |
| `0x45B03623` | GPU particle | misc_lightbeamexplode | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x4552F603` | GPU particle | misc_logtokencollect | bundle 01ca49 |
| `0x4501E9F5` | GPU particle | misc_notepickupbronze | Banjoland, LOGBOX 720, Nutty Acres, Showdown Town, Terrarium of Terror, World of Sports, bundle 5d5c16, bundle 68f1b8, bundle 8c1797, bundle ea08c0, bundle ee8a91, bundle fa4b2d |
| `0x456E61C8` | GPU particle | misc_notepickupgold | Banjoland, LOGBOX 720, Nutty Acres, Showdown Town, Terrarium of Terror, World of Sports, bundle 5d5c16, bundle 68f1b8, bundle 8c1797, bundle ea08c0, bundle ee8a91, bundle fa4b2d |
| `0x45B24827` | GPU particle | misc_notepickupsilver | Banjoland, LOGBOX 720, Nutty Acres, Showdown Town, Terrarium of Terror, World of Sports, bundle 5d5c16, bundle 68f1b8, bundle 8c1797, bundle ea08c0, bundle ee8a91, bundle fa4b2d |
| `0x457B8A1B` | GPU particle | misc_notesparklebronze | Banjoland, LOGBOX 720, Nutty Acres, Showdown Town, Terrarium of Terror, World of Sports, bundle 5d5c16, bundle 68f1b8, bundle 8c1797, bundle ea08c0, bundle ee8a91, bundle fa4b2d |
| `0x452C25AA` | GPU particle | misc_notesparklegold | Banjoland, LOGBOX 720, Nutty Acres, Showdown Town, Terrarium of Terror, World of Sports, bundle 01ca49, bundle 5d5c16, bundle 68f1b8, bundle 8c1797, bundle ea08c0, bundle ee8a91, bundle fa4b2d |
| `0x45C82BC9` | GPU particle | misc_notesparklesilver | Banjoland, LOGBOX 720, Nutty Acres, Showdown Town, Terrarium of Terror, World of Sports, bundle 5d5c16, bundle 68f1b8, bundle 8c1797, bundle ea08c0, bundle ee8a91, bundle fa4b2d |
| `0x45F852E6` | GPU particle | misc_patchleak | bundle 959f49, bundle eaaac4 |
| `0x45922D30` | GPU particle | misc_steam | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x45233D63` | GPU particle | misc_targetexplosionsmoke | bundle c94cec, bundle eaaac4 |

## particle

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x1B92EB73` | emitter | particle_egg_explodedust | anywhere (common) |
| `0x1C92EB73` | particle | particle_egg_explodedust | anywhere (common) |
| `0x1B9DF1AB` | emitter | particle_egg_explodeflash | anywhere (common) |
| `0x1C9DF1AB` | particle | particle_egg_explodeflash | anywhere (common) |

## surfaceimpacts

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x4B3196E1` | composite | surfaceimpacts_fabric | anywhere (common) |
| `0x4B7596D4` | composite | surfaceimpacts_fabricsmall | anywhere (common) |
| `0x45EA99CA` | GPU particle | surfaceimpacts_generaldust | anywhere (common) |
| `0x45D66654` | GPU particle | surfaceimpacts_generaldustimpact | anywhere (common) |
| `0x45CAD4D8` | GPU particle | surfaceimpacts_generaldustimpactsmall | anywhere (common) |
| `0x4529B69A` | GPU particle | surfaceimpacts_generalsnow | anywhere (common) |
| `0x45C30516` | GPU particle | surfaceimpacts_generalsnowbits | anywhere (common) |
| `0x45822D46` | GPU particle | surfaceimpacts_generalsnowbitsimpact | anywhere (common) |
| `0x45CD1E90` | GPU particle | surfaceimpacts_generalsnowimpact | anywhere (common) |
| `0x4554A0D8` | GPU particle | surfaceimpacts_jetsmalldirectdust | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45D3C679` | GPU particle | surfaceimpacts_jetsmalldirectdustless | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x456EB82A` | GPU particle | surfaceimpacts_jetsmalldirectsand | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45978F88` | GPU particle | surfaceimpacts_jetsmalldirectsnow | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x458CB6BB` | GPU particle | surfaceimpacts_jetsmallindirectdust | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45DF1E6B` | GPU particle | surfaceimpacts_jetsmallindirectdustless | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45B6AE49` | GPU particle | surfaceimpacts_jetsmallindirectsand | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x454F99EB` | GPU particle | surfaceimpacts_jetsmallindirectsnow | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4B562CB5` | composite | surfaceimpacts_metal | anywhere (common) |
| `0x4BBF37C4` | composite | surfaceimpacts_metalsmall | anywhere (common) |
| `0x4501835E` | GPU particle | surfaceimpacts_mudimpact | anywhere (common) |
| `0x45CB325B` | GPU particle | surfaceimpacts_mudslide | anywhere (common) |
| `0x450F63DC` | GPU particle | surfaceimpacts_proplargeindirectdust | Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x451EFB01` | GPU particle | surfaceimpacts_proplargeindirectdustless | Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x45CC4C8C` | GPU particle | surfaceimpacts_proplargeindirectsnow | Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x455446E2` | GPU particle | surfaceimpacts_propsmalldirectdust | Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4590B024` | GPU particle | surfaceimpacts_propsmalldirectdustless | Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x459769B2` | GPU particle | surfaceimpacts_propsmalldirectsnow | Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4B5D7088` | composite | surfaceimpacts_snow | anywhere (common) |
| `0x4BF42698` | composite | surfaceimpacts_snowslide | anywhere (common) |
| `0x4B43CC36` | composite | surfaceimpacts_wood | anywhere (common) |
| `0x4B5A034E` | composite | surfaceimpacts_woodsmall | anywhere (common) |

## vehicle

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x4507D6F1` | GPU particle | vehicle_disabledsmoke | anywhere (common) |
| `0x4A2574AB` | 3D GPU particle | vehicle_disabledsparks | anywhere (common) |
| `0x45FA0375` | GPU particle | vehicle_empdisabledsmoke | anywhere (common) |
| `0x45A7CE47` | GPU particle | vehicle_empdisabledsparks | anywhere (common) |
| `0x1B8D99D7` | emitter | vehicle_engine_jetthrust | anywhere (common) |
| `0x1C8D99D7` | particle | vehicle_engine_jetthrust | anywhere (common) |
| `0x1B03ABF9` | emitter | vehicle_firegun | anywhere (common) |
| `0x1C03ABF9` | particle | vehicle_firegun | anywhere (common) |
| `0x451DB142` | GPU particle | vehicle_invulnerable | anywhere (common) |
| `0x45C55D36` | GPU particle | vehicle_mpgainingpoints | anywhere (common) |
| `0x1B7E623F` | emitter | vehicle_skidlarge | anywhere (common) |
| `0x1C7E623F` | particle | vehicle_skidlarge | anywhere (common) |
| `0x1B50A192` | emitter | vehicle_skidsmall | anywhere (common) |
| `0x1C50A192` | particle | vehicle_skidsmall | anywhere (common) |
| `0x1B45EB4D` | emitter | vehicle_towbarattach | anywhere (common) |
| `0x1C45EB4D` | particle | vehicle_towbarattach | anywhere (common) |
| `0x45D8EDEE` | GPU particle | vehicle_vehicleblur | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x4AF703E5` | 3D GPU particle | vehicle_wheels_standard_fabricimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4A23E9EB` | 3D GPU particle | vehicle_wheels_standard_fabricimpactsmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4A05C806` | 3D GPU particle | vehicle_wheels_standard_woodimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |

## vehicleblocks

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x4516AF27` | GPU particle | vehicleblocks_blockbreakoffsmoketrail | anywhere (common) |
| `0x1CD51740` | particle | vehicleblocks_cirusslick_expiresplat | bundle 4e967e, bundle e66c1c |
| `0x1CE3AD68` | particle | vehicleblocks_cirusslick_splatexplode | bundle 4e967e, bundle e66c1c |
| `0x1B5F70BF` | emitter | vehicleblocks_citrusslick_expiresplat | bundle 4e967e, bundle e66c1c |
| `0x1B71C882` | emitter | vehicleblocks_citrusslick_splatexplode | bundle 4e967e, bundle e66c1c |
| `0x458B5018` | GPU particle | vehicleblocks_colouredsmoketrail1 | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x458201A2` | GPU particle | vehicleblocks_colouredsmoketrail2 | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45C5CADD` | GPU particle | vehicleblocks_enginelargebubbletrail | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45664C5A` | GPU particle | vehicleblocks_enginelargeflamesmall | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x450BF72B` | GPU particle | vehicleblocks_enginelargeflare | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45B04A4B` | GPU particle | vehicleblocks_enginelargeheattrail | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45E6D8EF` | GPU particle | vehicleblocks_enginelargestartsmoke | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45A704DD` | GPU particle | vehicleblocks_enginemediumflare | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x452A863E` | GPU particle | vehicleblocks_enginemediumrunningsmoke | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45A94A78` | GPU particle | vehicleblocks_enginemediumstartsmoke | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x454EA618` | GPU particle | vehicleblocks_enginesmallbubbletrail | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45A41CC0` | GPU particle | vehicleblocks_enginesuperbubbletrail | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4512965E` | GPU particle | vehicleblocks_enginesuperrunning | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45BD4FC7` | GPU particle | vehicleblocks_enginesuperstartup | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45106493` | GPU particle | vehicleblocks_insidesmokesphere | bundle 4e967e |
| `0x45625EB0` | GPU particle | vehicleblocks_jetlargebubbletrail | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45FF8C57` | GPU particle | vehicleblocks_jetlargeflame | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45F35063` | GPU particle | vehicleblocks_jetlargeflameboost | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45C29489` | GPU particle | vehicleblocks_jetlargeheattrail | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45FECD03` | GPU particle | vehicleblocks_jetsmallflameboost | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45EC366A` | GPU particle | vehicleblocks_jetsmallheattrail | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4582E7D5` | GPU particle | vehicleblocks_jetsmoke1 | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x458BB66F` | GPU particle | vehicleblocks_jetsmoke2 | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x452E197D` | GPU particle | vehicleblocks_largepropflare | bundle 4e967e, bundle e66c1c |
| `0x45044E1E` | GPU particle | vehicleblocks_largepropstallsmoke | bundle 4e967e, bundle e66c1c |
| `0x4560297B` | GPU particle | vehicleblocks_largepropstartsmoke | bundle 4e967e, bundle e66c1c |
| `0x452B796A` | GPU particle | vehicleblocks_largepropstartsparks | bundle 4e967e, bundle e66c1c |
| `0x45943710` | GPU particle | vehicleblocks_liquidsquirter | bundle 4e967e |
| `0x45BFD5D3` | GPU particle | vehicleblocks_liquidsquirter2 | bundle 4e967e |
| `0x451E4521` | GPU particle | vehicleblocks_proplargebubbletrail | Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x451C171F` | GPU particle | vehicleblocks_replenishing | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x459DE88E` | GPU particle | vehicleblocks_robofixrepair | bundle 4e967e |
| `0x4543BC01` | GPU particle | vehicleblocks_sailwind | bundle 4e967e, bundle e66c1c |
| `0x45E43F37` | GPU particle | vehicleblocks_skidsmoke | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x454264FB` | GPU particle | vehicleblocks_smallpropstallsmoke | Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x456E5A45` | GPU particle | vehicleblocks_smallpropstartupsmoke | Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4543B68D` | GPU particle | vehicleblocks_smokesphere | bundle 4e967e |
| `0x45F6C7B6` | GPU particle | vehicleblocks_springimpact | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x458E09D4` | GPU particle | vehicleblocks_suckerblowerbubbles | bundle 4e967e |
| `0x45400BAB` | GPU particle | vehicleblocks_superfuel | streamed only |
| `0x457AFD38` | GPU particle | vehicleblocks_unpaintblock | anywhere (common) |
| `0x458FBAB8` | GPU particle | vehicleblocks_vacuumblowout | anywhere (common) |
| `0x45CDD117` | GPU particle | vehicleblocks_vacuumblowoutdust | anywhere (common) |
| `0x457F13C4` | GPU particle | vehicleblocks_vacuumsuckin | anywhere (common) |
| `0x455EFCA9` | GPU particle | vehicleblocks_vapourtrail | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x457AAC73` | GPU particle | vehicleblocks_waterimpactbubbles | anywhere (common) |
| `0x45BD9F7E` | GPU particle | vehicleblocks_wheels_monster_dustmovebig | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x453F1BDA` | GPU particle | vehicleblocks_wheels_monster_dustmovebigless | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x452FCC1F` | GPU particle | vehicleblocks_wheels_monster_dustmovesmall | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x454C317D` | GPU particle | vehicleblocks_wheels_monster_dustmovesmallless | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x454582AF` | GPU particle | vehicleblocks_wheels_monster_snowmovebig | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x450AC860` | GPU particle | vehicleblocks_wheels_monster_snowmovesmall | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45F8405C` | GPU particle | vehicleblocks_wheels_standard_dustimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4563B556` | GPU particle | vehicleblocks_wheels_standard_dustimpactbigless | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45F1273F` | GPU particle | vehicleblocks_wheels_standard_dustimpactsmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4514CA89` | GPU particle | vehicleblocks_wheels_standard_dustimpactsmallless | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x459E293C` | GPU particle | vehicleblocks_wheels_standard_dustmovebig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45351491` | GPU particle | vehicleblocks_wheels_standard_dustmovebigless | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45627D02` | GPU particle | vehicleblocks_wheels_standard_dustmovesmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x457EF928` | GPU particle | vehicleblocks_wheels_standard_dustmovesmallless | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x455E3E79` | GPU particle | vehicleblocks_wheels_standard_sandimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45BF2F45` | GPU particle | vehicleblocks_wheels_standard_sandimpactsmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45224DEB` | GPU particle | vehicleblocks_wheels_standard_sandmovebig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45C40327` | GPU particle | vehicleblocks_wheels_standard_sandmovesmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45BABE3F` | GPU particle | vehicleblocks_wheels_standard_snowbitsimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4524A602` | GPU particle | vehicleblocks_wheels_standard_snowbitsimpactsmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45208361` | GPU particle | vehicleblocks_wheels_standard_snowbitsmovesmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45AACF65` | GPU particle | vehicleblocks_wheels_standard_snowdustimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45723066` | GPU particle | vehicleblocks_wheels_standard_snowdustimpactsmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x456634ED` | GPU particle | vehicleblocks_wheels_standard_snowmovebig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4547797D` | GPU particle | vehicleblocks_wheels_standard_snowmovesmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x452BF148` | GPU particle | vehicleblocks_wheels_super_dustimpactbig | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4539BAA3` | GPU particle | vehicleblocks_wheels_super_dustimpactsmall | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45E39990` | GPU particle | vehicleblocks_wheels_super_dustmovebig | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45B1CC16` | GPU particle | vehicleblocks_wheels_super_dustmovesmall | Mumbo's Motors, bundle 4e967e, bundle e66c1c |

## vehicleparts

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x4B9FF1CD` | composite | vehicleparts_ramimpact | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4BBD2B39` | composite | vehicleparts_wheels_standard_fabricimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4B1B3448` | composite | vehicleparts_wheels_standard_fabricimpactsmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4B707839` | composite | vehicleparts_wheels_standard_snowimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4B7971CB` | composite | vehicleparts_wheels_standard_snowimpactsmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4BEA4567` | composite | vehicleparts_wheels_standard_snowmovesmall | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4BF5642B` | composite | vehicleparts_wheels_standard_woodimpactbig | Mumbo's Motors, Showdown Town, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |

## water

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x47FFC8B8` | blobs droplet | water | bundle 4e967e |
| `0x45E52AF4` | GPU particle | water_airbubbles | anywhere (common) |
| `0x45FA86F4` | GPU particle | water_cogspray | bundle 01ede7, bundle 04dd5f, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle 90aff1, bundle c94cec, bundle cc7c54 |
| `0x45599D03` | GPU particle | water_cogspray2 | bundle 01ede7, bundle 04dd5f, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle 90aff1, bundle c94cec, bundle cc7c54 |
| `0x4560B286` | GPU particle | water_fountaindroplets1 | World of Sports |
| `0x45CA1EEA` | GPU particle | water_plankton | anywhere (common) |
| `0x454590F8` | GPU particle | water_plankton2 | anywhere (common) |
| `0x45841959` | GPU particle | water_vehiclesplashdroplets | anywhere (common) |
| `0x45ACB9C1` | GPU particle | water_vehiclewakedroplets | anywhere (common) |
| `0x450A65AC` | GPU particle | water_vehiclewakedropletssmall | anywhere (common) |
| `0x4591FC00` | GPU particle | water_vehiclewakespray | anywhere (common) |
| `0x458BECC5` | GPU particle | water_vehiclewakespraysmall | anywhere (common) |

## weapon

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x45D40130` | GPU particle | weapon_bigbomb_flame | Banjo's house, bundle 04dd5f, bundle 90aff1, bundle eaaac4 |
| `0x45ACF42B` | GPU particle | weapon_bigbomb_smoke | bundle 04dd5f, bundle 90aff1, bundle eaaac4 |
| `0x45CDA7D3` | GPU particle | weapon_citrusslick_expiredust | bundle 4e967e, bundle e66c1c |
| `0x4566281C` | GPU particle | weapon_citrusslick_explodedust | bundle 4e967e, bundle e66c1c |
| `0x45C193B4` | GPU particle | weapon_citrusslick_firesmoke | bundle 4e967e, bundle e66c1c |
| `0x45C26CC3` | GPU particle | weapon_clockworkkazooie_trailflame | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4A32E945` | 3D GPU particle | weapon_egg_shards1 | Spiral Mountain / title screen, bundle 4527d5, bundle 4e967e, bundle b9ddc8, bundle e66c1c |
| `0x4A3BB8FF` | 3D GPU particle | weapon_egg_shards2 | Spiral Mountain / title screen, bundle 4527d5, bundle 4e967e, bundle b9ddc8, bundle e66c1c |
| `0x45662AAA` | GPU particle | weapon_egggun_dusttrail | Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x459F6ECE` | GPU particle | weapon_egggun_hitsmoke1 | Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4505F908` | GPU particle | weapon_egggun_lava1 | Nutty Acres |
| `0x45DAEA9D` | GPU particle | weapon_egggun_lavasmoke1 | Nutty Acres |
| `0x45D28707` | GPU particle | weapon_egggun_lavasplash1 | Nutty Acres |
| `0x45DBD6BD` | GPU particle | weapon_egggun_lavasplash2 | Nutty Acres |
| `0x45DCE62B` | GPU particle | weapon_egggun_lavasplash3 | Nutty Acres |
| `0x455A6B17` | GPU particle | weapon_egggun_lavasplashparent1 | Nutty Acres |
| `0x45E2C2DC` | GPU particle | weapon_egggun_lavasplashsmall1 | Nutty Acres |
| `0x45FF93D8` | GPU particle | weapon_egggun_lavatrail2 | Nutty Acres |
| `0x454FC5E1` | GPU particle | weapon_emp_endsmoke | Mumbo's Motors, bundle 4e967e |
| `0x45781142` | GPU particle | weapon_freezeezy_crystalshatter | anywhere (common) |
| `0x4506FD79` | GPU particle | weapon_freezeezy_fire | bundle 4e967e, bundle e66c1c |
| `0x45F5CF5F` | GPU particle | weapon_freezeezy_fire1 | bundle 4e967e, bundle e66c1c |
| `0x45FC9EE5` | GPU particle | weapon_freezeezy_fire2 | bundle 4e967e, bundle e66c1c |
| `0x4529207B` | GPU particle | weapon_freezeezy_frozendryice | anywhere (common) |
| `0x45122CA6` | GPU particle | weapon_freezeezy_frozensparkles | anywhere (common) |
| `0x4585B821` | GPU particle | weapon_freezeezy_hit1 | bundle 4e967e, bundle e66c1c |
| `0x458CE99B` | GPU particle | weapon_freezeezy_hit2 | bundle 4e967e, bundle e66c1c |
| `0x45A72C56` | GPU particle | weapon_freezeezy_hitsmoke | bundle 4e967e, bundle e66c1c |
| `0x45E47484` | GPU particle | weapon_freezeezy_shatterdust | anywhere (common) |
| `0x45C2A44F` | GPU particle | weapon_freezeezy_trail | bundle 4e967e, bundle e66c1c |
| `0x45B6F6D9` | GPU particle | weapon_grenade_explode1 | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x45665DB7` | GPU particle | weapon_grenade_explodebitsfortrail | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x451F08FF` | GPU particle | weapon_grenade_explodesmoke | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x45A51785` | GPU particle | weapon_grenade_explodesmoke10m | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x4531640A` | GPU particle | weapon_grenade_explodetrail | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x455EB7B8` | GPU particle | weapon_grenade_fireflash | anywhere (common) |
| `0x45EC6533` | GPU particle | weapon_grenade_fireflash1 | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4521C771` | GPU particle | weapon_grenade_projectilesmoketrail | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x4568C808` | GPU particle | weapon_grenade_projectilesparktrail | Mumbo's Motors, Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x4598248F` | GPU particle | weapon_gun_muzzleflash | Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4583F121` | GPU particle | weapon_gun_starflash | Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4519EB09` | GPU particle | weapon_laser_fire | Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x4543A175` | GPU particle | weapon_laser_fire1 | Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x4575F161` | GPU particle | weapon_laser_hitexplode | Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x4557A26D` | GPU particle | weapon_laser_hitsmoke1 | Showdown Town, bundle 4e967e, bundle e66c1c |
| `0x456AB59B` | GPU particle | weapon_mumbobombo_dusttrail | bundle 4e967e, bundle e66c1c |
| `0x45CAB3B8` | GPU particle | weapon_mumbobombo_expire | bundle 4e967e, bundle e66c1c |
| `0x45C4F137` | GPU particle | weapon_mumbobombo_expire1 | bundle 4e967e, bundle e66c1c |
| `0x457E9DBA` | GPU particle | weapon_mumbobombo_explodebitsfortrail | bundle 4e967e, bundle e66c1c |
| `0x455B36A4` | GPU particle | weapon_mumbobombo_explodetrail | bundle 4e967e, bundle e66c1c |
| `0x45B27886` | GPU particle | weapon_mumbobombo_explosion | bundle 4e967e, bundle e66c1c |
| `0x45C89457` | GPU particle | weapon_mumbobombo_explosion1 | bundle 4e967e, bundle e66c1c |
| `0x4580F16B` | GPU particle | weapon_mumbobombo_explosionbits | bundle 4e967e, bundle e66c1c |
| `0x457D600D` | GPU particle | weapon_mumbobombo_fire | bundle 4e967e, bundle e66c1c |
| `0x450D35CF` | GPU particle | weapon_ram_hitbig | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45814DA5` | GPU particle | weapon_ram_impactring | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45131A93` | GPU particle | weapon_rustbin_expire | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4531A324` | GPU particle | weapon_rustbin_explodesmoke | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x452DC73E` | GPU particle | weapon_rustbin_fire | Banjoland, LOGBOX 720, Terrarium of Terror, bundle 01ede7, bundle 10e7bb, bundle 10ef2f, bundle 45a42d, bundle 47e9f3, bundle 4db709, bundle 5d5c16, bundle 616298, bundle 67b0d3, bundle adcb8b, bundle ba2e23, bundle ea08c0, bundle ea6e2d, bundle ee8a91, bundle fd97da |
| `0x4589CB6B` | GPU particle | weapon_rustbin_firesmoke | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4AC04EB8` | 3D GPU particle | weapon_rustbin_rustbits1 | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4AE0A465` | 3D GPU particle | weapon_rustbin_rustbitsexplode | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4A8F7549` | 3D GPU particle | weapon_rustbin_rustbitsexplode1 | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4ADE46BE` | 3D GPU particle | weapon_rustbin_rustbitsfire | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4A0840F7` | 3D GPU particle | weapon_rustbin_rustbitsfire1 | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4AEED0F6` | 3D GPU particle | weapon_rustbin_sparks | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x4A4A1DB1` | 3D GPU particle | weapon_rustbin_trailbits | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x459D3745` | GPU particle | weapon_rustbin_trailnew | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x45C5CF59` | GPU particle | weapon_spikehitflash | Mumbo's Motors, bundle 4e967e, bundle e66c1c |
| `0x458CD16C` | GPU particle | weapon_torpedile_explodebitsfortrail | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x459B660F` | GPU particle | weapon_torpedile_explodesmoke | anywhere (common) |
| `0x45B13700` | GPU particle | weapon_torpedile_explodesparks | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45B50AFA` | GPU particle | weapon_torpedile_explodetrail | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45916857` | GPU particle | weapon_torpedile_explosion | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x4AAA4B85` | 3D GPU particle | weapon_torpedile_explosionsparks | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45C4F1E9` | GPU particle | weapon_torpedile_fire1 | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45D9A4B4` | GPU particle | weapon_torpedile_heathazeimpactring | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45F39AF9` | GPU particle | weapon_torpedile_trail | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45F25A15` | GPU particle | weapon_torpedile_trailflame | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |
| `0x45E36F6D` | GPU particle | weapon_torpedile_ventsmoke | Mumbo's Motors, Spiral Mountain / title screen, bundle 4e967e, bundle e66c1c |

## other

| ID | Type | Name | Loaded in |
|---|---|---|---|
| `0x4B681B8F` | composite | banjowatershake1 | anywhere (common) |
| `0x4B7C8BCF` | composite | bbqfireandsmoke | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x4B51C818` | composite | bbqsmokefaraway1 | Nutty Acres |
| `0x4B431DC2` | composite | beancandust1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle e66c1c, bundle ee8a91, bundle fd97da |
| `0x4AB9A5FF` | 3D GPU particle | blockfalloffsparks | anywhere (common) |
| `0x4BBD0025` | composite | bombimpact1 | bundle 04dd5f, bundle 90aff1, bundle e66c1c, bundle eaaac4 |
| `0x4B5AF603` | composite | bugzapper1 | anywhere (common) |
| `0x4BA0218E` | composite | buttonpressedsteam1 | Banjoland, bundle 0b6509, bundle 10ef2f, bundle 38aba7, bundle 4db709, bundle 9a271f, bundle 9f17a7, bundle adcb8b, bundle ba2e23, bundle c3c402, bundle c6f4ba, bundle ee8a91, bundle fd97da |
| `0x4B31A227` | composite | cpufanwind1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x4BCCD21E` | composite | deedsmagic1 | streamed only |
| `0x4BA3D328` | composite | dicedestroy1 | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4BB1A7C0` | composite | digitalnumbers1 | LOGBOX 720 |
| `0x4B6AB5EE` | composite | digitalnumbersblue | LOGBOX 720 |
| `0x4B3F0E8C` | composite | digitalnumberspink | LOGBOX 720 |
| `0x4B33824F` | composite | digitalnumbersyellow | LOGBOX 720 |
| `0x4A43B03F` | 3D GPU particle | dragsparks | anywhere (common) |
| `0x4A0ABCBF` | 3D GPU particle | dragsparkssmall | anywhere (common) |
| `0x4B0268CF` | composite | eggexplode1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x4B38CDFD` | composite | energyball1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba |
| `0x4A5CB6D4` | 3D GPU particle | fabricimpact | anywhere (common) |
| `0x4A894AB2` | 3D GPU particle | fabricimpactsmall | anywhere (common) |
| `0x4B05C347` | composite | fanunitglow1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba, bundle e66c1c |
| `0x4B1450EE` | composite | faultywires1 | Banjoland, Showdown Town, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x4B92DFA7` | composite | firewall1 | bundle e66c1c |
| `0x4B82F7C8` | composite | flubberdrip1 | LOGBOX 720 |
| `0x4B5F0B5E` | composite | flubbergush1 | LOGBOX 720 |
| `0x4BF69291` | composite | gameglobe1 | Showdown Town |
| `0x4B0710FB` | composite | garagetorch1 | Mumbo's Motors |
| `0x4B92CEC0` | composite | gruntbotexplosion | bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086 |
| `0x4B370CA9` | composite | gruntbotsmashpipe1 | Banjo's house, bundle 0b6509, bundle 1533cd, bundle 38aba7, bundle 49e068, bundle 959f49, bundle 9a271f, bundle b349d9, bundle b67961, bundle c3c402, bundle c6f4ba, bundle c94cec, bundle e66c1c |
| `0x4B5D88E5` | composite | hoopboostjet1 | bundle 34a511, bundle 6dc100, bundle e66c1c |
| `0x4BFACF40` | composite | hoophoverjet1 | bundle 34a511, bundle 6dc100, bundle e66c1c |
| `0x4B363076` | composite | iceblockslide1 | bundle 7be8d2, bundle e66c1c |
| `0x4B180ACC` | composite | icebreaker1 | Banjoland |
| `0x4A8251F7` | 3D GPU particle | iceshatter | bundle 7be8d2 |
| `0x4A4CE5D4` | 3D GPU particle | iceshatter1 | anywhere (common) |
| `0x4BBAE942` | composite | iceslidemain1 | bundle 7be8d2, bundle e66c1c |
| `0x4A386A3D` | 3D GPU particle | impactsparks | anywhere (common) |
| `0x4AAD77A1` | 3D GPU particle | impactsparkssmall | anywhere (common) |
| `0x4B6D368B` | composite | jingotransform1 | anywhere (common) |
| `0x4B413CE1` | composite | kazooiefeathers1 | anywhere (common) |
| `0x4BFB615D` | composite | lavaburst1 | Nutty Acres |
| `0x4BF230E7` | composite | lavaburst2 | Nutty Acres |
| `0x4B310AB9` | composite | leaffall1 | Banjoland |
| `0x47002395` | blobs droplet | leakypipe | bundle 4e967e |
| `0x4BE01824` | composite | logappear1 | Spiral Mountain, Spiral Mountain / title screen |
| `0x4BE29149` | composite | logolympicsteam1 | bundle 4e967e, bundle e66c1c |
| `0x4BEA486F` | composite | logolympictorch1 | bundle 4e967e, bundle e66c1c |
| `0x4B321575` | composite | missilebeam1 | Banjoland |
| `0x4B7D67E1` | composite | mrpatchdrip1 | bundle e66c1c, bundle eaaac4 |
| `0x4BA351D1` | composite | mrpatchsplash1 | Banjoland |
| `0x4B73AAA3` | composite | mumbocrateexplode1 | bundle e66c1c |
| `0x4B4A6274` | composite | mumbomagichands1 | anywhere (common) |
| `0x4BD8CF03` | composite | mumboswitchactivate1 | Banjoland, Showdown Town, bundle 0b6509, bundle 10ef2f, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 38aba7, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 4db709, bundle 8c1797, bundle 9a271f, bundle 9cedca, bundle 9f17a7, bundle 9f19ad, bundle adcb8b, bundle b21454, bundle ba2e23, bundle be1cf4, bundle c016b4, bundle c3c402, bundle c6f4ba, bundle cd43ee, bundle e37086, bundle ee8a91, bundle fd97da |
| `0x47FEC914` | blobs droplet | paintsplosh | anywhere (common) |
| `0x4B39ECA0` | composite | piddlestrail1 | Banjo's house, Mumbo's Motors, Showdown Town, bundle 100375, bundle 68f1b8, bundle 8c1797, bundle b67961, bundle c3c402, bundle c94cec, bundle cc7c54, bundle e66c1c |
| `0x4B8EF4DE` | composite | pillevaporate1 | bundle 0b6509, bundle 38aba7, bundle 9a271f, bundle 9f17a7, bundle c3c402, bundle c6f4ba |
| `0x4B33AF43` | composite | pipepressuresteam1 | Spiral Mountain, Spiral Mountain / title screen, bundle 17b0fb, bundle 181575, bundle 34a511, bundle 3ee0fd, bundle 3ef9bf, bundle 455e80, bundle 4cff52, bundle 8c1797, bundle 9cedca, bundle 9f19ad, bundle b21454, bundle be1cf4, bundle c016b4, bundle cd43ee, bundle e37086, bundle e66c1c |
| `0x4B4F5FDC` | composite | pipepressuresteambig1 | Banjoland |
| `0x4B3EDF72` | composite | pizzacrumbs1 | Mumbo's Motors, bundle 01ca49, bundle e66c1c |
| `0x1BD087D5` | emitter | projectile_citrusslick_explodeflash | Nutty Acres, bundle 10b565, bundle 3ee0fd, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x1B5E4857` | emitter | projectile_emp_explodeflash | Mumbo's Motors, bundle 3a1b12, bundle 4e967e |
| `0x4BC60457` | composite | rockimpact1 | Mumbo's Motors, bundle 01ca49, bundle 90aff1, bundle e66c1c |
| `0x4B4D2BE1` | composite | rockpoisongas1 | bundle 90aff1, bundle e66c1c |
| `0x4BDC108D` | composite | rocktrail1 | Nutty Acres |
| `0x4B97312C` | composite | snowfall1 | Banjoland |
| `0x4B79E875` | composite | snowmachine1 | Banjoland, bundle 10ef2f, bundle 4db709, bundle adcb8b, bundle ba2e23, bundle ee8a91, bundle fd97da |
| `0x1BA9C30E` | emitter | stepsplash_water_default | anywhere (common) |
| `0x1CA9C30E` | particle | stepsplash_water_default | anywhere (common) |
| `0x47943FA3` | blobs droplet | terrorium_sprinkler | Terrarium of Terror, bundle 47e9f3, bundle ea08c0, bundle ea6e2d |
| `0x47002C38` | blobs droplet | terrorium_sprinklerhits | Terrarium of Terror, bundle 47e9f3, bundle ea08c0, bundle ea6e2d |
| `0x4BEEA6A1` | composite | torchfirebig | World of Sports |
| `0x4BA396CF` | composite | ttshing1 | bundle 3112a5, bundle 34221d, bundle 6dc100 |
| `0x48A965D3` | explosion | vehicleexplosion | anywhere (common) |
| `0x479B17F6` | blobs droplet | vehiclesplash | anywhere (common) |
| `0x47CBADFB` | blobs droplet | vehiclewake | anywhere (common) |
| `0x4BDA5CB5` | composite | volcanoexplosion1 | Nutty Acres |
| `0x4B94102F` | composite | volcanofire | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x4B421F3F` | composite | volcanopipe1 | Nutty Acres, bundle 10b565, bundle 45a42d, bundle 64010e, bundle 67b0d3, bundle fa4b2d |
| `0x4B546727` | composite | waterdrip1 | Banjoland |
| `0x47C3F505` | blobs droplet | waternohits | bundle 4e967e |
| `0x4A7267EB` | 3D GPU particle | woodimpact | anywhere (common) |
| `0x4A4953CC` | 3D GPU particle | woodimpactsmall | anywhere (common) |
| `0x4B2D7AB2` | composite | wrenchidle1 | anywhere (common) |
| `0x4B107335` | composite | wrenchmagic1 | anywhere (common) |
