// Minimal deterministic libretro runner used by the donor trace probes.
// It normally discards video/audio samples: the instrumented core writes
// emulated GPU/CD/SPU/GTE events directly to PS1_DONOR_TRACE.  A selected
// software-rendered video frame can be retained as a PPM donor oracle.

#include <libretro.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::map<std::string, std::string> options;
std::map<std::string, std::string> overrides;
std::string systemDirectory;
std::string saveDirectory;
retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
std::string dumpPath;
unsigned dumpFrame = ~0u;
std::string dumpDirectory;
unsigned dumpFrom = 0;
unsigned dumpTo = ~0u;
unsigned dumpStep = 1;
unsigned lastRangeDumpFrame = ~0u;
std::string statsPath;
std::FILE *statsFile = nullptr;
unsigned runFrame = 0;
bool frameDumped = false;
std::FILE *ramTraceFile = nullptr;
std::vector<uint32_t> ramTraceAddresses;
unsigned ramTraceFrom = 0;
unsigned ramTraceTo = ~0u;
unsigned ramTraceStep = 1;
uint64_t ramTraceSequence = 0;
std::string ramDumpDirectory;
std::vector<uint32_t> ramDumpFrames;
struct InputInterval {
  unsigned firstFrame = 0;
  unsigned lastFrame = 0;
  unsigned port = 0;
  unsigned button = 0;
};
std::vector<InputInterval> inputIntervals;
std::string stateLoadPath;
std::string stateDumpPath;
unsigned stateDumpFrame = ~0u;
bool stateDumped = false;
const auto hostEpoch = std::chrono::steady_clock::now();

template <typename T> T symbol(void *library, const char *name) {
  auto result = reinterpret_cast<T>(dlsym(library, name));
  if (!result)
    throw std::runtime_error(std::string("missing libretro symbol: ") + name);
  return result;
}

void log(enum retro_log_level level, const char *format, ...) {
  static const char *names[] = {"debug", "info", "warn", "error"};
  std::fprintf(stderr, "[libretro:%s] ", names[level <= RETRO_LOG_ERROR ? level : 0]);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
}

void rememberLegacyVariables(const retro_variable *variables) {
  if (!variables)
    return;
  for (const retro_variable *variable = variables; variable->key; ++variable) {
    const char *separator = variable->value ? std::strchr(variable->value, ';') : nullptr;
    if (!separator)
      continue;
    separator++;
    while (*separator == ' ')
      separator++;
    const char *end = std::strchr(separator, '|');
    options.try_emplace(variable->key, separator,
                        end ? static_cast<size_t>(end - separator)
                            : std::strlen(separator));
  }
}

void rememberDefinitions(const retro_core_option_definition *definitions) {
  if (!definitions)
    return;
  for (const auto *definition = definitions; definition->key; ++definition)
    if (definition->default_value)
      options.try_emplace(definition->key, definition->default_value);
}

void rememberDefinitionsV2(const retro_core_option_v2_definition *definitions) {
  if (!definitions)
    return;
  for (const auto *definition = definitions; definition->key; ++definition)
    if (definition->default_value)
      options.try_emplace(definition->key, definition->default_value);
}

bool environment(unsigned command, void *data) {
  switch (command) {
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    static_cast<retro_log_callback *>(data)->log = log;
    return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
    *static_cast<const char **>(data) = systemDirectory.c_str();
    return true;
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *static_cast<const char **>(data) = saveDirectory.c_str();
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    pixelFormat = *static_cast<const retro_pixel_format *>(data);
    return true;
  case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
  case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
  case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
  case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
  case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
  case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
  case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
  case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
  case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
    return true;
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    *static_cast<bool *>(data) = true;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    // This experimental query is a capability probe; libretro cores pass null.
    return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
  case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
    *static_cast<bool *>(data) = false;
    return true;
  case RETRO_ENVIRONMENT_GET_LANGUAGE:
    *static_cast<unsigned *>(data) = RETRO_LANGUAGE_ENGLISH;
    return true;
  case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
    *static_cast<unsigned *>(data) = 2;
    return true;
  case RETRO_ENVIRONMENT_SET_VARIABLES:
    rememberLegacyVariables(static_cast<const retro_variable *>(data));
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    rememberDefinitions(static_cast<const retro_core_option_definition *>(data));
    return true;
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
    const auto *intl = static_cast<const retro_core_options_intl *>(data);
    rememberDefinitions(intl ? intl->us : nullptr);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
    const auto *v2 = static_cast<const retro_core_options_v2 *>(data);
    rememberDefinitionsV2(v2 ? v2->definitions : nullptr);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
    const auto *intl = static_cast<const retro_core_options_v2_intl *>(data);
    rememberDefinitionsV2(intl && intl->us ? intl->us->definitions : nullptr);
    return true;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    auto *variable = static_cast<retro_variable *>(data);
    const auto overrideIt = overrides.find(variable->key);
    const auto defaultIt = options.find(variable->key);
    variable->value = overrideIt != overrides.end()
                          ? overrideIt->second.c_str()
                          : defaultIt != options.end() ? defaultIt->second.c_str() : nullptr;
    return variable->value != nullptr;
  }
  case RETRO_ENVIRONMENT_SET_MESSAGE: {
    const auto *message = static_cast<const retro_message *>(data);
    if (message && message->msg)
      std::fprintf(stderr, "[libretro:message] %s\n", message->msg);
    return true;
  }
  default:
    return false;
  }
}

void video(const void *data, unsigned width, unsigned height, size_t pitch) {
  const bool dumpSingle = !frameDumped && !dumpPath.empty() && runFrame == dumpFrame;
  const bool dumpRange = !dumpDirectory.empty() && runFrame >= dumpFrom &&
                         runFrame <= dumpTo &&
                         (runFrame - dumpFrom) % dumpStep == 0 &&
                         lastRangeDumpFrame != runFrame;
  if ((!dumpSingle && !dumpRange && !statsFile) || !data ||
      data == RETRO_HW_FRAME_BUFFER_VALID || width == 0 || height == 0)
    return;

  std::string rangePath;
  if (dumpRange)
    rangePath = (std::filesystem::path(dumpDirectory) /
                 ("frame-" + std::to_string(runFrame) + ".ppm"))
                    .string();
  const std::string &outputPath = dumpSingle ? dumpPath : rangePath;
  std::FILE *out = outputPath.empty() ? nullptr : std::fopen(outputPath.c_str(), "wb");
  if (!outputPath.empty() && !out)
    throw std::runtime_error("cannot create donor frame dump: " + outputPath);
  if (out)
    std::fprintf(out, "P6\n%u %u\n255\n", width, height);

  uint64_t hash = UINT64_C(14695981039346656037);
  uint64_t hashRgb5 = UINT64_C(14695981039346656037);
  uint64_t nonBlack = 0;

  for (unsigned y = 0; y < height; ++y) {
    const auto *row = static_cast<const uint8_t *>(data) + y * pitch;
    for (unsigned x = 0; x < width; ++x) {
      uint8_t rgb[3]{};
      if (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        const uint32_t pixel = reinterpret_cast<const uint32_t *>(row)[x];
        rgb[0] = static_cast<uint8_t>(pixel >> 16);
        rgb[1] = static_cast<uint8_t>(pixel >> 8);
        rgb[2] = static_cast<uint8_t>(pixel);
      } else {
        const uint16_t pixel = reinterpret_cast<const uint16_t *>(row)[x];
        if (pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
          rgb[0] = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
          rgb[1] = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
          rgb[2] = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
        } else {
          rgb[0] = static_cast<uint8_t>(((pixel >> 10) & 0x1f) * 255 / 31);
          rgb[1] = static_cast<uint8_t>(((pixel >> 5) & 0x1f) * 255 / 31);
          rgb[2] = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
        }
      }
      for (uint8_t component : rgb) {
        hash ^= component;
        hash *= UINT64_C(1099511628211);
        hashRgb5 ^= component >> 3;
        hashRgb5 *= UINT64_C(1099511628211);
      }
      nonBlack += rgb[0] != 0 || rgb[1] != 0 || rgb[2] != 0;
      if (out)
        std::fwrite(rgb, sizeof(rgb), 1, out);
    }
  }
  if (out)
    std::fclose(out);
  if (dumpSingle)
    frameDumped = true;
  if (dumpRange)
    lastRangeDumpFrame = runFrame;
  if (statsFile) {
    std::fprintf(statsFile, "%u,%u,%u,%016" PRIx64 ",%016" PRIx64
                            ",%" PRIu64 ",%" PRIu64 "\n",
                 runFrame, width, height, hash, hashRgb5, nonBlack,
                 static_cast<uint64_t>(width) * height);
    std::fflush(statsFile);
  }
}
void audio(int16_t, int16_t) {}
size_t audioBatch(const int16_t *, size_t frames) { return frames; }
void inputPoll() {}

unsigned inputButtonId(const std::string &name) {
  static const std::map<std::string, unsigned> buttons = {
      {"cross", RETRO_DEVICE_ID_JOYPAD_B},
      {"circle", RETRO_DEVICE_ID_JOYPAD_A},
      {"square", RETRO_DEVICE_ID_JOYPAD_Y},
      {"triangle", RETRO_DEVICE_ID_JOYPAD_X},
      {"start", RETRO_DEVICE_ID_JOYPAD_START},
      {"select", RETRO_DEVICE_ID_JOYPAD_SELECT},
      {"up", RETRO_DEVICE_ID_JOYPAD_UP},
      {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
      {"left", RETRO_DEVICE_ID_JOYPAD_LEFT},
      {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
      {"l1", RETRO_DEVICE_ID_JOYPAD_L},
      {"r1", RETRO_DEVICE_ID_JOYPAD_R},
      {"l2", RETRO_DEVICE_ID_JOYPAD_L2},
      {"r2", RETRO_DEVICE_ID_JOYPAD_R2},
      {"l3", RETRO_DEVICE_ID_JOYPAD_L3},
      {"r3", RETRO_DEVICE_ID_JOYPAD_R3},
  };
  const auto found = buttons.find(name);
  if (found == buttons.end())
    throw std::runtime_error("unknown PS1 donor input button: " + name);
  return found->second;
}

void loadInputScript(const char *path) {
  std::ifstream source(path);
  if (!source)
    throw std::runtime_error(std::string("cannot open donor input script: ") +
                             path);
  std::string line;
  unsigned lineNumber = 0;
  while (std::getline(source, line)) {
    ++lineNumber;
    const auto comment = line.find('#');
    if (comment != std::string::npos)
      line.resize(comment);
    std::istringstream fields(line);
    unsigned first = 0;
    unsigned last = 0;
    unsigned port = 0;
    std::string button;
    if (!(fields >> first))
      continue;
    if (!(fields >> last >> button))
      throw std::runtime_error("invalid donor input script line " +
                               std::to_string(lineNumber));
    if (fields >> port) {
      if (port > 7)
        throw std::runtime_error("invalid donor input port on line " +
                                 std::to_string(lineNumber));
    }
    std::string trailing;
    if (fields >> trailing || last < first)
      throw std::runtime_error("invalid donor input interval on line " +
                               std::to_string(lineNumber));
    inputIntervals.push_back({first, last, port, inputButtonId(button)});
  }
  std::fprintf(stderr, "[donor-input] loaded %zu interval(s) from %s\n",
               inputIntervals.size(), path);
}

uint16_t activeInputMask(unsigned port) {
  uint16_t mask = 0;
  for (const InputInterval &interval : inputIntervals) {
    if (interval.port == port && runFrame >= interval.firstFrame &&
        runFrame <= interval.lastFrame)
      mask |= static_cast<uint16_t>(1u << interval.button);
  }
  return mask;
}

int16_t inputState(unsigned port, unsigned device, unsigned, unsigned id) {
  if ((device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
    return 0;
  const uint16_t mask = activeInputMask(port);
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
    return static_cast<int16_t>(mask);
  return id < 16 && (mask & (1u << id)) != 0 ? 1 : 0;
}

std::vector<uint32_t> parseAddressList(const char *text) {
  std::vector<uint32_t> result;
  const char *cursor = text;
  while (cursor && *cursor) {
    char *end = nullptr;
    const unsigned long address = std::strtoul(cursor, &end, 0);
    if (end == cursor)
      throw std::runtime_error("invalid address in PS1_DONOR_RAM_ADDRESSES");
    result.push_back(static_cast<uint32_t>(address));
    cursor = end;
    while (*cursor == ',' || *cursor == ' ' || *cursor == '\t')
      ++cursor;
  }
  return result;
}

void sampleRam(const uint8_t *ram, size_t ramSize) {
  if (!ramTraceFile || !ram || runFrame < ramTraceFrom || runFrame > ramTraceTo ||
      (runFrame - ramTraceFrom) % ramTraceStep != 0)
    return;
  const char *source = std::getenv("PS1_DONOR_TRACE_SOURCE");
  if (!source || !*source)
    source = "libretro-donor";
  for (const uint32_t address : ramTraceAddresses) {
    const uint32_t offset = address & 0x1fffffffu;
    if (offset > ramSize || ramSize - offset < sizeof(uint32_t))
      throw std::runtime_error("RAM sample address is outside system RAM");
    uint32_t value = 0;
    std::memcpy(&value, ram + offset, sizeof(value));
    std::fprintf(ramTraceFile,
                 "{\"schema\":\"ps1trace/v1\",\"source\":\"%s\","
                 "\"seq\":%" PRIu64 ",\"frame\":%u,"
                 "\"host_us\":%" PRIu64 ","
                 "\"event\":\"diagnostic.ram_sample\","
                 "\"address\":%u,\"value\":%u}\n",
                 source, ++ramTraceSequence, runFrame,
                 static_cast<uint64_t>(
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - hostEpoch)
                         .count()),
                 address, value);
  }
  std::fflush(ramTraceFile);
}

void dumpRam(const uint8_t *ram, size_t ramSize) {
  if (ramDumpDirectory.empty() || !ram ||
      std::find(ramDumpFrames.begin(), ramDumpFrames.end(), runFrame) ==
          ramDumpFrames.end())
    return;
  const std::string path =
      (std::filesystem::path(ramDumpDirectory) /
       ("ram-frame-" + std::to_string(runFrame) + ".bin"))
          .string();
  std::FILE *out = std::fopen(path.c_str(), "wb");
  if (!out)
    throw std::runtime_error("cannot create donor RAM dump: " + path);
  const size_t written = std::fwrite(ram, 1, ramSize, out);
  std::fclose(out);
  if (written != ramSize)
    throw std::runtime_error("short donor RAM dump: " + path);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s CORE.so CONTENT.cue FRAMES [core-option=value ...]\n",
                 argv[0]);
    return 2;
  }

  const std::filesystem::path core = std::filesystem::absolute(argv[1]);
  const std::filesystem::path content = std::filesystem::absolute(argv[2]);
  const unsigned frames = static_cast<unsigned>(std::stoul(argv[3]));
  if (const char *path = std::getenv("PS1_DONOR_DUMP_PATH"))
    dumpPath = path;
  if (const char *frame = std::getenv("PS1_DONOR_DUMP_FRAME"))
    dumpFrame = static_cast<unsigned>(std::stoul(frame));
  if (const char *directory = std::getenv("PS1_DONOR_DUMP_DIRECTORY"))
    dumpDirectory = directory;
  if (const char *frame = std::getenv("PS1_DONOR_DUMP_FROM"))
    dumpFrom = static_cast<unsigned>(std::stoul(frame));
  if (const char *frame = std::getenv("PS1_DONOR_DUMP_TO"))
    dumpTo = static_cast<unsigned>(std::stoul(frame));
  if (const char *step = std::getenv("PS1_DONOR_DUMP_STEP"))
    dumpStep = static_cast<unsigned>(std::stoul(step));
  if (dumpStep == 0)
    throw std::runtime_error("PS1_DONOR_DUMP_STEP must be greater than zero");
  if (!dumpDirectory.empty())
    std::filesystem::create_directories(dumpDirectory);
  if (const char *path = std::getenv("PS1_DONOR_FRAME_STATS")) {
    statsPath = path;
    statsFile = std::fopen(statsPath.c_str(), "wb");
    if (!statsFile)
      throw std::runtime_error("cannot create donor frame stats: " + statsPath);
    std::fprintf(statsFile,
                 "frame,width,height,fnv1a_rgb8,fnv1a_rgb5,nonblack_pixels,total_pixels\n");
  }
  if (const char *path = std::getenv("PS1_DONOR_RAM_TRACE")) {
    const char *addresses = std::getenv("PS1_DONOR_RAM_ADDRESSES");
    if (!addresses || !*addresses)
      throw std::runtime_error(
          "PS1_DONOR_RAM_TRACE requires PS1_DONOR_RAM_ADDRESSES");
    ramTraceAddresses = parseAddressList(addresses);
    ramTraceFile = std::fopen(path, "wb");
    if (!ramTraceFile)
      throw std::runtime_error("cannot create donor RAM trace");
    if (const char *frame = std::getenv("PS1_DONOR_RAM_TRACE_FROM"))
      ramTraceFrom = static_cast<unsigned>(std::stoul(frame));
    if (const char *frame = std::getenv("PS1_DONOR_RAM_TRACE_TO"))
      ramTraceTo = static_cast<unsigned>(std::stoul(frame));
    if (const char *step = std::getenv("PS1_DONOR_RAM_TRACE_STEP"))
      ramTraceStep = static_cast<unsigned>(std::stoul(step));
    if (ramTraceStep == 0)
      throw std::runtime_error("PS1_DONOR_RAM_TRACE_STEP must be greater than zero");
  }
  if (const char *directory = std::getenv("PS1_DONOR_RAM_DUMP_DIRECTORY")) {
    const char *frames = std::getenv("PS1_DONOR_RAM_DUMP_FRAMES");
    if (!frames || !*frames)
      throw std::runtime_error(
          "PS1_DONOR_RAM_DUMP_DIRECTORY requires "
          "PS1_DONOR_RAM_DUMP_FRAMES");
    ramDumpDirectory = directory;
    ramDumpFrames = parseAddressList(frames);
    std::filesystem::create_directories(ramDumpDirectory);
  }
  if (const char *path = std::getenv("PS1_DONOR_INPUT_SCRIPT"))
    loadInputScript(path);
  if (const char *path = std::getenv("PS1_DONOR_STATE_LOAD"))
    stateLoadPath = path;
  if (const char *path = std::getenv("PS1_DONOR_STATE_DUMP_PATH")) {
    const char *frame = std::getenv("PS1_DONOR_STATE_DUMP_FRAME");
    if (!frame || !*frame)
      throw std::runtime_error(
          "PS1_DONOR_STATE_DUMP_PATH requires PS1_DONOR_STATE_DUMP_FRAME");
    stateDumpPath = path;
    stateDumpFrame = static_cast<unsigned>(std::stoul(frame));
  }
  for (int i = 4; i < argc; ++i) {
    const std::string setting = argv[i];
    const auto separator = setting.find('=');
    if (separator == std::string::npos)
      throw std::runtime_error("core option must be key=value: " + setting);
    overrides[setting.substr(0, separator)] = setting.substr(separator + 1);
  }

  const char* workOverride = std::getenv("PS1_DONOR_WORK_DIRECTORY");
  const auto work = workOverride && *workOverride
      ? std::filesystem::absolute(workOverride)
      : std::filesystem::temp_directory_path() / "ps1trace-libretro";
  systemDirectory = (work / "system").string();
  saveDirectory = (work / "save").string();
  std::filesystem::create_directories(systemDirectory);
  std::filesystem::create_directories(saveDirectory);

  void *library = dlopen(core.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library)
    throw std::runtime_error(dlerror());

  const auto setEnvironment = symbol<void (*)(retro_environment_t)>(library, "retro_set_environment");
  const auto setVideo = symbol<void (*)(retro_video_refresh_t)>(library, "retro_set_video_refresh");
  const auto setAudio = symbol<void (*)(retro_audio_sample_t)>(library, "retro_set_audio_sample");
  const auto setAudioBatch = symbol<void (*)(retro_audio_sample_batch_t)>(library, "retro_set_audio_sample_batch");
  const auto setInputPoll = symbol<void (*)(retro_input_poll_t)>(library, "retro_set_input_poll");
  const auto setInputState = symbol<void (*)(retro_input_state_t)>(library, "retro_set_input_state");
  const auto init = symbol<void (*)()>(library, "retro_init");
  const auto loadGame = symbol<bool (*)(const retro_game_info *)>(library, "retro_load_game");
  const auto run = symbol<void (*)()>(library, "retro_run");
  const auto serializeSize =
      symbol<size_t (*)()>(library, "retro_serialize_size");
  const auto serialize =
      symbol<bool (*)(void *, size_t)>(library, "retro_serialize");
  const auto unserialize =
      symbol<bool (*)(const void *, size_t)>(library, "retro_unserialize");
  const auto getMemoryData =
      symbol<void *(*)(unsigned)>(library, "retro_get_memory_data");
  const auto getMemorySize =
      symbol<size_t (*)(unsigned)>(library, "retro_get_memory_size");
  const auto unloadGame = symbol<void (*)()>(library, "retro_unload_game");
  const auto deinit = symbol<void (*)()>(library, "retro_deinit");

  setEnvironment(environment);
  setVideo(video);
  setAudio(audio);
  setAudioBatch(audioBatch);
  setInputPoll(inputPoll);
  setInputState(inputState);
  init();

  const std::string contentString = content.string();
  const retro_game_info game{contentString.c_str(), nullptr, 0, nullptr};
  if (!loadGame(&game)) {
    deinit();
    dlclose(library);
    throw std::runtime_error("retro_load_game failed for " + contentString);
  }

  if (!stateLoadPath.empty()) {
    std::ifstream state(stateLoadPath, std::ios::binary | std::ios::ate);
    if (!state)
      throw std::runtime_error("cannot open donor state: " + stateLoadPath);
    const std::streamsize size = state.tellg();
    if (size <= 0)
      throw std::runtime_error("empty donor state: " + stateLoadPath);
    state.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!state.read(reinterpret_cast<char *>(bytes.data()), size) ||
        !unserialize(bytes.data(), bytes.size()))
      throw std::runtime_error("core rejected donor state: " + stateLoadPath);
    std::fprintf(stderr, "[donor-state] loaded %zu bytes from %s\n",
                 bytes.size(), stateLoadPath.c_str());
  }

  const bool benchmark = [] {
    const char *value = std::getenv("PS1_DONOR_BENCHMARK");
    return value != nullptr && *value != '\0' && *value != '0';
  }();
  const auto runLoopStart = std::chrono::steady_clock::now();
  for (unsigned frame = 0; frame < frames; ++frame) {
    runFrame = frame + 1;
    run();
    const auto *ram = static_cast<const uint8_t *>(
        getMemoryData(RETRO_MEMORY_SYSTEM_RAM));
    const size_t ramSize = getMemorySize(RETRO_MEMORY_SYSTEM_RAM);
    sampleRam(ram, ramSize);
    dumpRam(ram, ramSize);
    if (!stateDumped && !stateDumpPath.empty() &&
        runFrame == stateDumpFrame) {
      const size_t size = serializeSize();
      if (size == 0)
        throw std::runtime_error("core reports an empty serialized state");
      std::vector<uint8_t> bytes(size);
      if (!serialize(bytes.data(), bytes.size()))
        throw std::runtime_error("core failed to serialize donor state");
      std::ofstream state(stateDumpPath,
                          std::ios::binary | std::ios::trunc);
      if (!state || !state.write(reinterpret_cast<const char *>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot write donor state: " +
                                 stateDumpPath);
      stateDumped = true;
      std::fprintf(stderr, "[donor-state] wrote %zu bytes to %s at frame %u\n",
                   bytes.size(), stateDumpPath.c_str(), runFrame);
    }
  }
  const auto runLoopEnd = std::chrono::steady_clock::now();
  if (benchmark) {
    const uint64_t elapsedUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(runLoopEnd -
                                                              runLoopStart)
            .count());
    const double framesPerSecond =
        elapsedUs != 0 ? static_cast<double>(frames) * 1000000.0 /
                             static_cast<double>(elapsedUs)
                       : 0.0;
    std::fprintf(stderr,
                 "[donor-benchmark] %u frames in %" PRIu64
                 " us (%.2f/s)\n",
                 frames, elapsedUs, framesPerSecond);
  }

  if (!stateDumpPath.empty() && !stateDumped)
    throw std::runtime_error("donor state dump frame was not reached");

  unloadGame();
  deinit();
  dlclose(library);
  if (statsFile)
    std::fclose(statsFile);
  if (ramTraceFile)
    std::fclose(ramTraceFile);
  return 0;
}
