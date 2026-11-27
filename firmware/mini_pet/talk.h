// Types shared by the fish-talk code in mini_pet.ino (kept here so Arduino's generated prototypes can see them).
#pragma once
#include <Arduino.h>
#include "pet.h"

struct Names { char name[FISH][16]; char trait[FISH][40]; };

enum { JOB_SAY, JOB_NAMES, JOB_DIARY, JOB_CHAT, JOB_PUSH };
struct Job {
  int type, fish, sit;
  char text[240], title[48];
  SemaphoreHandle_t done;  // JOB_CHAT: the web handler waits on it and owns the job
  char reply[200];
};
