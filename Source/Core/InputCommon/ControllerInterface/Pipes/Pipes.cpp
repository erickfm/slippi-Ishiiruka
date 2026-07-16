// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Pipes/Pipes.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/State.h"

namespace ciface
{
namespace Pipes
{
static const std::array<std::string, 12> s_button_tokens{
    {"A", "B", "X", "Y", "Z", "START", "L", "R", "D_UP", "D_DOWN", "D_LEFT", "D_RIGHT"}};

static const std::array<std::string, 2> s_shoulder_tokens{{"L", "R"}};

static const std::array<std::string, 2> s_axis_tokens{{"MAIN", "C"}};

static double StringToDouble(const std::string& text)
{
  std::istringstream is(text);
  // ignore current locale
  is.imbue(std::locale::classic());
  double result;
  is >> result;
  return result;
}

void PopulateDevices()
{
  #ifdef _WIN32
  PIPE_FD pipes[4];
  // Windows has named pipes, but they're different. They don't exist on the
  //  local filesystem and are transient. So rather than searching the /Pipes
  //  directory for pipes, we just always assume there's 4 and then make them
  for (uint32_t i = 0; i < 4; i++)
  {
    std::string pipename = "\\\\.\\pipe\\slippibot" + std::to_string(i+1);
    pipes[i] = CreateNamedPipeA(
       pipename.data(),              // pipe name
       PIPE_ACCESS_INBOUND,          // read access, inward only
       PIPE_TYPE_BYTE | PIPE_NOWAIT, // byte mode, nonblocking
       1,                            // number of clients
       256,                          // output buffer size
       256,                          // input buffer size
       0,                            // timeout value
       NULL                          // security attributes
    );

    // We're in nonblocking mode, so this won't wait for clients
    ConnectNamedPipe(pipes[i], NULL);
    std::string ui_pipe_name = "slippibot" + std::to_string(i+1);
    g_controller_interface.AddDevice(std::make_shared<PipeDevice>(pipes[i], ui_pipe_name));
  }
  #else

  // Search the Pipes directory for files that we can open in read-only,
  // non-blocking mode. The device name is the virtual name of the file.
  File::FSTEntry fst;
  std::string dir_path = File::GetUserPath(D_PIPES_IDX);
  if (!File::Exists(dir_path))
    return;
  fst = File::ScanDirectoryTree(dir_path, false);
  if (!fst.isDirectory)
    return;
  for (unsigned int i = 0; i < fst.size; ++i)
  {
    const File::FSTEntry& child = fst.children[i];
    if (child.isDirectory)
      continue;
    PIPE_FD fd = open(child.physicalName.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    g_controller_interface.AddDevice(std::make_shared<PipeDevice>(fd, child.virtualName));
  }
  #endif
}

static bool s_pipe_trace = getenv("PIPE_TRACE") != nullptr;
static std::vector<PipeDevice*> s_pipe_registry;

PipeDevice::PipeDevice(PIPE_FD fd, const std::string& name) : m_fd(fd), m_name(name)
{
  // libmelee names bot pipes "slippibot<port>" (1-based). Knowing the port
  // lets UpdateInput consult the per-port player type for frame syncing.
  if (m_name.rfind("slippibot", 0) == 0 && m_name.size() == 10 &&
      m_name[9] >= '1' && m_name[9] <= '4')
    m_slippi_port = m_name[9] - '1';
  s_pipe_registry.push_back(this);
  for (const auto& tok : s_button_tokens)
  {
    PipeInput* btn = new PipeInput("Button " + tok);
    AddInput(btn);
    m_buttons[tok] = btn;
  }
  for (const auto& tok : s_shoulder_tokens)
  {
    AddAxis(tok, 0.0);
  }
  for (const auto& tok : s_axis_tokens)
  {
    AddAxis(tok + " X", 0.5);
    AddAxis(tok + " Y", 0.5);
  }
}

PipeDevice::~PipeDevice()
{
  s_pipe_registry.erase(
      std::remove(s_pipe_registry.begin(), s_pipe_registry.end(), this),
      s_pipe_registry.end());
  #ifdef _WIN32
  CloseHandle(m_fd);
  #else
  close(m_fd);
  #endif
}

s32 PipeDevice::readFromPipe(PIPE_FD file_descriptor, char *in_buffer, size_t size)
{
  #ifdef _WIN32

  u32 bytes_available = 0;
  DWORD bytesread = 0;
  bool peek_success = PeekNamedPipe(
    file_descriptor,
    NULL,
    0,
    NULL,
    (LPDWORD)&bytes_available,
    NULL
  );

  if(!peek_success && (GetLastError() == ERROR_BROKEN_PIPE))
  {
    DisconnectNamedPipe(file_descriptor);
    ConnectNamedPipe(file_descriptor, NULL);
    return -1;
  }

  if(peek_success && (bytes_available > 0))
  {
    bool success = ReadFile(
      file_descriptor,    // pipe handle
      in_buffer,          // buffer to receive reply
      (DWORD)std::min(bytes_available, (u32)size),        // size of buffer
      &bytesread,         // number of bytes read
      NULL);              // not overlapped
    if(!success)
    {
        return -1;
    }
  }
  return (s32)bytesread;
  #else
  return read(file_descriptor, in_buffer, size);
  #endif
}

// Read everything currently available and parse all complete commands.
// Returns false if the pipe died (writer closed), true otherwise.
//
// In barrier mode, each parsed FLUSH is classified against the frame-sync
// ledger (see the strict-mode comment below):
//   - the first m_tail_expect flushes are the previous cycle's outstanding
//     keep-alive tail — consumed, never a barrier;
//   - the next flush satisfies the frame barrier (*barrier = true). If it
//     carried data (non-FLUSH commands since the previous flush), the
//     client's final keep-alive for this cycle is still outstanding, so
//     m_tail_expect is incremented for the next frame;
//   - further flushes in the same drain re-enter the same ledger (e.g. the
//     keep-alive right behind a data burst cancels the increment).
bool PipeDevice::DrainAndParse(bool barrier_mode, bool* barrier)
{
  char buf[32];
  bool died = false;
  s32 bytes_read = readFromPipe(m_fd, buf, sizeof buf);
  if (bytes_read == 0)
    died = true;  // writer closed; parse what we have, then bail
  while (bytes_read > 0)
  {
    m_buf.append(buf, bytes_read);
    bytes_read = readFromPipe(m_fd, buf, sizeof buf);
    if (bytes_read == 0)
      died = true;
  }
  std::size_t newline = m_buf.find("\n");
  while (newline != std::string::npos)
  {
    std::string command = m_buf.substr(0, newline);
    bool is_flush = ParseCommand(command);
    if (is_flush)
    {
      m_flushes_seen++;
      if (barrier_mode)
      {
        if (m_tail_expect > 0)
          m_tail_expect--;
        else if (barrier && !*barrier)
        {
          *barrier = true;
          if (m_last_flush_had_data)
            m_tail_expect++;
        }
        // else: extra bare flush beyond the barrier — ignore.
      }
    }
    m_buf.erase(0, newline + 1);
    newline = m_buf.find("\n");
  }
  return !died;
}

// --- Frame-synced ("strict") input mode -------------------------------------
//
// Used in-game for pipes that drive a HUMAN port when blocking pipes are on.
// It fixes a dual-pad input desync under fast-forward.
//
// Background: libmelee's console.step() writes one bare keep-alive FLUSH to
// EVERY controller pipe at the start of every step, in addition to the bot's
// real "commands + FLUSH" burst written between steps. The legacy per-device
// wait ends on the FIRST flush it sees, so a keep-alive flush left over from
// the previous frame can satisfy the per-frame blocking wait before the
// bot's real burst for this frame has arrived. The 0xD9 pad snapshot then
// serves stale inputs, the fresh burst is consumed mid-frame by the
// (non-blocking) 240 Hz SI polls, and each port independently oscillates
// between on-time and one-frame-late input — ~40-60% of frames landed late
// in dual-pad FFW self-play, destroying frame-tight play (L-cancels etc).
// Single-pad vs-CPU setups dodge the race by accident of device iteration
// order, and realtime dodges it because the 16.7 ms frame lets the SI polls
// mop up the keep-alive before the next frame's wait.
//
// Strict mode instead runs ONE combined wait per frame across ALL strict
// pipes, triggered by whichever strict device the frame's first input sweep
// reaches first (g_slippiFrameEpoch identifies the frame). Per client cycle
// (= its response to one gamestate) a pipe carries, in order: the bot's
// command burst ending in a data-carrying FLUSH (if the bot acted), then the
// bare keep-alive FLUSH from console.step()'s flush-all. The barrier for
// frame f must accept exactly one cycle per frame and must not let cycle
// f-1's trailing keep-alive stand in for cycle f. Each pipe keeps a ledger
// (m_tail_expect) of how many trailing keep-alives of the accepted cycle are
// still outstanding:
//   - flushes covered by m_tail_expect are the previous cycle's tail —
//     consumed, never a barrier;
//   - the first flush beyond the tail satisfies this frame's barrier,
//     whether it was already buffered (the client ran ahead of the wait —
//     normal in realtime pacing, or when a legacy blocking pipe earlier in
//     the sweep already absorbed the client round-trip) or arrives during
//     the wait (normal in FFW, where the wait starts microseconds after the
//     bookend). A data-carrying barrier flush leaves one keep-alive
//     outstanding (m_tail_expect++); a bare one IS the keep-alive;
//   - extra flushes drained behind the barrier flush re-enter the ledger
//     (the keep-alive right behind a data burst cancels the increment).
//
// Outside the combined wait, strict pipes consume nothing (mid-frame polls
// are no-ops), so cycle tails stay buffered for the next barrier's ledger
// instead of being stripped mid-frame at unpredictable times.
//
// Pipes on CPU/empty ports are excluded (legacy path): the game ignores
// their pads, and their pipes may receive nothing but keep-alives.
//
// Safety valve: a bounded wait accepts any flush consumed within this
// barrier call (even ledger-classified tail) rather than deadlocking, e.g.
// for clients with unusual multi-flush cycles that momentarily confuse the
// ledger. A pipe that produced no flush at all keeps blocking, as before.

static u32 s_served_epoch = ~0u;

bool PipeDevice::IsStrict() const
{
  return SConfig::GetInstance().m_blockingPipes && g_slippiInGame &&
         m_slippi_port >= 0 && g_slippiPlayerType[m_slippi_port] == 0;
}

#ifndef _WIN32
static void CombinedFrameWait()
{
  std::vector<PipeDevice*> pipes;
  for (PipeDevice* d : s_pipe_registry)
    if (d->IsStrict())
      pipes.push_back(d);
  if (pipes.empty())
    return;

  const size_t n = pipes.size();
  std::vector<char> barrier(n, 0), dead(n, 0);
  std::vector<u32> flushes_at_entry(n);
  for (size_t i = 0; i < n; i++)
    flushes_at_entry[i] = pipes[i]->GetFlushesSeen();

  // Consume whatever is already buffered; the ledger inside DrainAndParse
  // decides whether it contains this frame's barrier flush.
  for (size_t i = 0; i < n; i++)
  {
    bool b = false;
    if (!pipes[i]->DrainAndParse(true, &b))
      dead[i] = 1;
    barrier[i] = b;
  }

  // Block until every live strict pipe has crossed its frame barrier.
  int waited_ms = 0;
  while (true)
  {
    fd_set set;
    FD_ZERO(&set);
    int maxfd = -1;
    bool pending = false;
    for (size_t i = 0; i < n; i++)
    {
      if (dead[i] || barrier[i])
        continue;
      int fd = pipes[i]->GetFD();
      FD_SET(fd, &set);
      if (fd > maxfd)
        maxfd = fd;
      pending = true;
    }
    if (!pending)
      break;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    int ret = select(maxfd + 1, &set, NULL, NULL, &tv);
    if (ret == 0)
    {
      waited_ms += 250;
      // Valve: release pipes that consumed at least one flush during this
      // barrier call even if the ledger classified it as tail.
      bool released = false;
      for (size_t i = 0; i < n; i++)
      {
        if (!dead[i] && !barrier[i] &&
            pipes[i]->GetFlushesSeen() != flushes_at_entry[i])
        {
          barrier[i] = 1;
          released = true;
        }
      }
      if (released)
        fprintf(stderr,
                "PipeDevice: combined frame wait timed out after %d ms; "
                "proceeding on tail flush(es)\n", waited_ms);
      continue;
    }
    for (size_t i = 0; i < n; i++)
    {
      if (dead[i] || barrier[i])
        continue;
      if (FD_ISSET(pipes[i]->GetFD(), &set))
      {
        bool b = false;
        if (!pipes[i]->DrainAndParse(true, &b))
          dead[i] = 1;
        barrier[i] = barrier[i] || b;
      }
    }
  }
  if (s_pipe_trace)
    for (size_t i = 0; i < n; i++)
      fprintf(stderr, "[PT] pipe=%s frame-wait flushes=%u barrier=%d dead=%d\n",
              pipes[i]->GetName().c_str(),
              pipes[i]->GetFlushesSeen() - flushes_at_entry[i],
              (int)barrier[i], (int)dead[i]);
}
#endif

void PipeDevice::UpdateInput()
{
  const bool blocking = SConfig::GetInstance().m_blockingPipes;
  const bool wait_for_inputs = blocking && g_needInputForFrame;

  #ifndef _WIN32
  if (IsStrict())
  {
    // Strict pipes consume only via the once-per-frame combined wait.
    if (g_needInputForFrame && s_served_epoch != g_slippiFrameEpoch)
    {
      s_served_epoch = g_slippiFrameEpoch;
      CombinedFrameWait();
    }
    return;
  }
  #endif

  if (s_pipe_trace)
    fprintf(stderr, "[PT] pipe=%s wait=%d strict=0\n", m_name.c_str(),
            (int)wait_for_inputs);

  int trace_cmds = 0, trace_flushes = 0;

  // Legacy path (menus, boot, non-blocking mode, CPU/unknown ports,
  // Windows): unchanged semantics.
  bool finished = false;
  #ifndef _WIN32
  if(wait_for_inputs)
  {
    fd_set set;
    FD_ZERO (&set);
    FD_SET (m_fd, &set);

    // Wait for activity on the socket
    select(m_fd+1, &set, NULL, NULL, NULL);
  }
  #endif
  do
  {
    // Read any pending characters off the pipe. If we hit a newline,
    // then dequeue a command off the front of m_buf and parse it.
    char buf[32];
    s32 bytes_read = readFromPipe(m_fd, buf, sizeof buf);
    if (bytes_read == 0) {
      // Pipe died, so just quit out
      return;
    }
    while (bytes_read > 0)
    {
      m_buf.append(buf, bytes_read);
      bytes_read = readFromPipe(m_fd, buf, sizeof buf);
    }
    std::size_t newline = m_buf.find("\n");
    while (newline != std::string::npos)
    {
      std::string command = m_buf.substr(0, newline);
      finished = ParseCommand(command);
      trace_cmds++;
      if (finished)
        trace_flushes++;

      m_buf.erase(0, newline + 1);
      newline = m_buf.find("\n");
    }
  } while(!finished && wait_for_inputs);
  if (s_pipe_trace)
    fprintf(stderr, "[PT] pipe=%s done cmds=%d flushes=%d strict=0\n",
            m_name.c_str(), trace_cmds, trace_flushes);
}

void PipeDevice::AddAxis(const std::string& name, double value)
{
  // Dolphin uses separate axes for left/right, which complicates things.
  PipeInput* ax_hi = new PipeInput("Axis " + name + " +");
  ax_hi->SetState(value);
  PipeInput* ax_lo = new PipeInput("Axis " + name + " -");
  ax_lo->SetState(value);
  m_axes[name + " +"] = ax_hi;
  m_axes[name + " -"] = ax_lo;
  AddAnalogInputs(ax_lo, ax_hi);
}

u8 FloatToU8(double value)
{
  // Match the empirical behavior of the named pipes.
  s8 raw = std::floor((value - 0.5) * 254);
  return reinterpret_cast<u8 &>(raw);
}

void PipeDevice::SetAxis(const std::string& entry, double value)
{
  value = MathUtil::Clamp(value, 0.0, 1.0);

  if (entry.compare("MAIN X") == 0)
  {
    m_current_pad.padBuf[2] = FloatToU8(value);
  }
  if (entry.compare("MAIN Y") == 0)
  {
    m_current_pad.padBuf[3] = FloatToU8(value);
  }
  if (entry.compare("C X") == 0)
  {
    m_current_pad.padBuf[4] = FloatToU8(value);
  }
  if (entry.compare("C Y") == 0)
  {
    m_current_pad.padBuf[5] = FloatToU8(value);
  }
  if (entry.compare("L") == 0)
  {
    m_current_pad.padBuf[6] = u8 (value * 255);
  }
  if (entry.compare("R") == 0)
  {
    m_current_pad.padBuf[7] = u8 (value * 255);
  }

  double hi = std::max(0.0, value - 0.5) * 2.0;
  double lo = (0.5 - std::min(0.5, value)) * 2.0;
  auto search_hi = m_axes.find(entry + " +");
  if (search_hi != m_axes.end())
    search_hi->second->SetState(hi);
  auto search_lo = m_axes.find(entry + " -");
  if (search_lo != m_axes.end())
    search_lo->second->SetState(lo);
}

bool PipeDevice::ParseCommand(const std::string& command)
{
  if(command == "FLUSH")
  {
    // Let ControllerInterface.cpp clear the flag after all PipeDevices
    // have been queried.
    // g_needInputForFrame = false;
    m_last_flush_had_data = m_cmds_since_flush > 0;
    m_cmds_since_flush = 0;
    return true;
  }
  m_cmds_since_flush++;
  std::vector<std::string> tokens;
  SplitString(command, ' ', tokens);
  if (tokens.size() < 2 || tokens.size() > 4)
    return false;
  // Savestate control over the bot pipe: "SAVESTATE <path>" /
  // "LOADSTATE <path>". State::SaveAs/LoadAs use Core::PauseAndLock and
  // must run on the host thread; UpdateInput runs on the CPU thread, so
  // queue a host job (MainNoGUI's platform loop dispatches them).
  if (tokens.size() == 2 && (tokens[0] == "SAVESTATE" || tokens[0] == "LOADSTATE"))
  {
    const std::string path = tokens[1];
    const bool save = (tokens[0] == "SAVESTATE");
    fprintf(stderr, "[PIPE-SS] queueing %s %s\n", tokens[0].c_str(), path.c_str());
    ::Core::QueueHostJob([path, save] {
      fprintf(stderr, "[PIPE-SS] host job running: %s %s\n",
              save ? "SAVE" : "LOAD", path.c_str());
      if (save)
        ::State::SaveAs(path, true);
      else
        ::State::LoadAs(path);
      fprintf(stderr, "[PIPE-SS] host job done: %s\n", save ? "SAVE" : "LOAD");
    });
    return false;
  }
  if (tokens[0] == "PRESS" || tokens[0] == "RELEASE")
  {
    SetButtonState(tokens[1], tokens[0]);
    auto search = m_buttons.find(tokens[1]);
    if (search != m_buttons.end())
      search->second->SetState(tokens[0] == "PRESS" ? 1.0 : 0.0);
  }
  else if (tokens[0] == "SET")
  {
    if (tokens.size() == 3)
    {
      double value = StringToDouble(tokens[2]);
      SetAxis(tokens[1], value);
    }
    else if (tokens.size() == 4)
    {
      double x = StringToDouble(tokens[2]);
      double y = StringToDouble(tokens[3]);
      SetAxis(tokens[1] + " X", x);
      SetAxis(tokens[1] + " Y", y);
    }
  }
  return false;
}

SlippiPad PipeDevice::GetSlippiPad()
{
  return m_current_pad;
}

void PipeDevice::SetButtonState(const std::string& button, const std::string& press)
{
  u8 mask = 0x00;
  int index = 0;
  bool is_press = press == "PRESS";

  if (button.compare("A") == 0)
  {
    mask = 0x01;
    index = 0;
  }
  if (button.compare("B") == 0)
  {
    mask = 0x02;
    index = 0;
  }
  if (button.compare("X") == 0)
  {
    mask = 0x04;
    index = 0;
  }
  if (button.compare("Y") == 0)
  {
    mask = 0x08;
    index = 0;
  }
  if (button.compare("L") == 0)
  {
    mask = 0x40;
    index = 1;
  }
  if (button.compare("R") == 0)
  {
    mask = 0x20;
    index = 1;
  }
  if (button.compare("START") == 0)
  {
    mask = 0x10;
    index = 0;
  }
  if (button.compare("D_LEFT") == 0)
  {
    mask = 0x01;
    index = 1;
  }
  if (button.compare("D_RIGHT") == 0)
  {
    mask = 0x02;
    index = 1;
  }
  if (button.compare("D_DOWN") == 0)
  {
    mask = 0x04;
    index = 1;
  }
  if (button.compare("D_UP") == 0)
  {
    mask = 0x08;
    index = 1;
  }
  if (button.compare("Z") == 0)
  {
    mask = 0x10;
    index = 1;
  }
  if (is_press)
  {
    m_current_pad.padBuf[index] |= mask;
  }
  else
  {
    m_current_pad.padBuf[index] &= ~(mask);
  }
}

}
}
