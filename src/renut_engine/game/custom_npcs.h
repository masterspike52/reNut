#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace renut::game::custom_npcs {

void Declare();

bool HasTexts();

bool FindText(const std::string &key, std::u16string *text);

uint32_t ActiveScene();

std::vector<uint32_t> VoiceWavebanks(uint32_t scene);

struct Progress {
  int jiggies = 0, jiggies_total = 0;
  int trophies = 0, trophies_total = 0;
  int notes = 0, notes_total = 0;
};

Progress WorldProgress(const uint8_t *base, const std::string &world);

void SetPlayerAvatar(uint32_t avatar);

std::string NpcTag(const std::string &world, const std::string &id);

std::string NpcObjparams(const std::string &world, const std::string &id);

struct NpcEntry {
  std::string world, id, name;
};

std::vector<NpcEntry> Npcs();

}
