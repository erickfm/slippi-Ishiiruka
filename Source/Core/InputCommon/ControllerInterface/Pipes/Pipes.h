// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "Core/Slippi/SlippiPad.h"

extern bool g_needInputForFrame;
extern bool g_slippiInGame;
extern u8 g_slippiPlayerType[4];
extern u32 g_slippiFrameEpoch;

namespace ciface
{
namespace Pipes
{
  #ifdef _WIN32
  typedef HANDLE PIPE_FD;
  #else
  typedef int PIPE_FD;
  #endif

// To create a piped controller input, create a named pipe in the
// Pipes directory and write commands out to it. Commands are separated
// by a newline character, with spaces separating command tokens.
// Command syntax is as follows, where curly brackets are one-of and square
// brackets are inclusive numeric ranges. Cases are sensitive. Numeric inputs
// are clamped to [0, 1] and otherwise invalid commands are discarded.
// {PRESS, RELEASE} {A, B, X, Y, Z, START, L, R, D_UP, D_DOWN, D_LEFT, D_RIGHT}
// SET {L, R} [0, 1]
// SET {MAIN, C} [0, 1] [0, 1]
// FLUSH -> In blocking input mode, this tells dolphin to progress to the next frame
//    (does nothing in non-blocking mode)
void PopulateDevices();

class PipeDevice : public Core::Device
{
public:
  PipeDevice(PIPE_FD fd, const std::string& name);
  ~PipeDevice();

  void UpdateInput() override;
  std::string GetName() const override { return m_name; }
  std::string GetSource() const override { return "Pipe"; }
  SlippiPad GetSlippiPad();
  // Frame-synced input mode support (see Pipes.cpp).
  bool IsStrict() const;
  bool DrainAndParse(bool barrier_mode, bool* barrier);
  PIPE_FD GetFD() const { return m_fd; }
  u32 GetFlushesSeen() const { return m_flushes_seen; }
private:
  class PipeInput : public Input
  {
  public:
    PipeInput(const std::string& name) : m_name(name), m_state(0.0) {}
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override { return m_state; }
    void SetState(ControlState state) { m_state = state; }
  private:
    const std::string m_name;
    ControlState m_state;
  };

  void AddAxis(const std::string& name, double value);
  bool ParseCommand(const std::string& command);
  void SetAxis(const std::string& entry, double value);
  s32 readFromPipe(PIPE_FD file_descriptor, char *in_buffer, size_t size);
  void SetButtonState(const std::string& button, const std::string& press);

  const PIPE_FD m_fd;
  const std::string m_name;
  // Controller port (0-3) this pipe drives, derived from the libmelee
  // naming convention "slippibot<port>"; -1 if the name doesn't match.
  int m_slippi_port = -1;
  // Frame-sync ledger (see Pipes.cpp): outstanding keep-alive flushes of
  // the last accepted cycle, command adjacency for bare-flush detection,
  // and a running flush counter for the barrier safety valve.
  int m_tail_expect = 0;
  int m_cmds_since_flush = 0;
  bool m_last_flush_had_data = false;
  u32 m_flushes_seen = 0;
  std::string m_buf;
  std::map<std::string, PipeInput*> m_buttons;
  std::map<std::string, PipeInput*> m_axes;
  SlippiPad m_current_pad;
};
}
}
