// Bad Apple player module (v5: grayscale).
// - Registers a Mod Menu module (master switch = the module's own on/off).
// - Draws 1-bit frames as filled rectangles via pl::modmenu::submitDrawCommands.
// - If resources/bad_apple.bap is not found next to the .so, it plays a built-in
//   test animation (bouncing white box) so you can check that drawing works.
// - Audio: plays resources/bad_apple.mp3 through android.media.MediaPlayer (JNI).
//
// .bap format: see tools/make_bap.py (BAP2 = grayscale with 2, 4 or 16 levels;
// old BAP1 = black/white is still readable).
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

#include <dlfcn.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr const char *kModuleId = "bad_apple.module";
constexpr const char *kBlockSizeKey = "blockSize";
constexpr const char *kFpsKey = "fps";
constexpr const char *kLoopKey = "loop";
constexpr const char *kSoundKey = "sound";
constexpr const char *kPosXKey = "posX";
constexpr const char *kPosYKey = "posY";
constexpr const char *kQualityKey = "quality";  // 0 = bad_apple.bap, 1 = bad_apple_240.bap

bool parseBool(std::string_view v, bool fallback) {
  if (v == "true" || v == "1" || v == "on") return true;
  if (v == "false" || v == "0" || v == "off") return false;
  return fallback;
}

// The menu may report a radio choice by its index ("0", "1") or by its label.
int parseQuality(std::string_view v, int fallback) {
  const std::string t(v);
  if (t.find("240") != std::string::npos || t.find("Medium") != std::string::npos) return 1;
  if (t.find("480") != std::string::npos || t.find("High") != std::string::npos) return 0;
  const char *p = t.c_str();
  char *end = nullptr;
  const long n = std::strtol(p, &end, 10);
  if (end == p || *end != '\0') return fallback;
  return n == 1 ? 1 : 0;
}

int parseInt(std::string_view v, int fallback) {
  std::string t(v);
  char *end = nullptr;
  errno = 0;
  const long p = std::strtol(t.c_str(), &end, 10);
  if (end == t.c_str() || *end != '\0' || errno == ERANGE) return fallback;
  return static_cast<int>(p);
}

std::string moduleDir() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(&moduleDir), &info) && info.dli_fname) {
    std::string p(info.dli_fname);
    const auto pos = p.find_last_of('/');
    if (pos != std::string::npos) return p.substr(0, pos);
  }
  return {};
}


// ---------------------------------------------------------------- JNI audio
JavaVM *gVm = nullptr;

JavaVM *findVm() {
  if (gVm) return gVm;
  using GetVmsFn = jint (*)(JavaVM **, jsize, jsize *);
  void *sym = dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
  if (!sym) {
    if (void *h = dlopen("libnativehelper.so", RTLD_NOW)) sym = dlsym(h, "JNI_GetCreatedJavaVMs");
  }
  if (!sym) {
    if (void *h = dlopen("libart.so", RTLD_NOW)) sym = dlsym(h, "JNI_GetCreatedJavaVMs");
  }
  if (!sym) return nullptr;
  JavaVM *vms[1] = {nullptr};
  jsize n = 0;
  if (reinterpret_cast<GetVmsFn>(sym)(vms, 1, &n) == JNI_OK && n > 0) gVm = vms[0];
  return gVm;
}

// Attaches the current thread to the JVM for the lifetime of the object.
struct JniScope {
  JavaVM *vm = nullptr;
  JNIEnv *env = nullptr;
  bool attached = false;
  JniScope() {
    vm = findVm();
    if (!vm) return;
    const jint rc = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
      if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
      else env = nullptr;
    } else if (rc != JNI_OK) {
      env = nullptr;
    }
  }
  ~JniScope() {
    if (attached && vm) vm->DetachCurrentThread();
  }
  JniScope(const JniScope &) = delete;
  JniScope &operator=(const JniScope &) = delete;
  explicit operator bool() const { return env != nullptr; }
};

bool clearException(JNIEnv *env) {
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return true;
  }
  return false;
}

jmethodID findMethod(JNIEnv *env, jclass cls, const char *name, const char *sig) {
  jmethodID m = env->GetMethodID(cls, name, sig);
  if (clearException(env)) return nullptr;
  return m;
}

// Thin wrapper around android.media.MediaPlayer.
class Audio {
public:
  bool start(const std::string &path) {
    std::lock_guard lock(mMutex);
    releaseLocked();
    JniScope jni;
    if (!jni) return false;
    JNIEnv *env = jni.env;

    jclass cls = env->FindClass("android/media/MediaPlayer");
    if (!cls || clearException(env)) return false;
    jmethodID ctor = findMethod(env, cls, "<init>", "()V");
    jobject local = ctor ? env->NewObject(cls, ctor) : nullptr;
    if (!local || clearException(env)) {
      env->DeleteLocalRef(cls);
      return false;
    }
    jobject player = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);

    bool ok = false;
    jmethodID setSource = findMethod(env, cls, "setDataSource", "(Ljava/lang/String;)V");
    jmethodID prepare = findMethod(env, cls, "prepare", "()V");
    jmethodID startM = findMethod(env, cls, "start", "()V");
    if (setSource && prepare && startM) {
      jstring js = env->NewStringUTF(path.c_str());
      env->CallVoidMethod(player, setSource, js);
      env->DeleteLocalRef(js);
      if (!clearException(env)) {
        env->CallVoidMethod(player, prepare);
        if (!clearException(env)) {
          env->CallVoidMethod(player, startM);
          ok = !clearException(env);
        }
      }
    }
    env->DeleteLocalRef(cls);
    if (!ok) {
      destroyPlayer(env, player);
      return false;
    }
    mPlayer = player;
    return true;
  }

  // Seek to the beginning and play again (used when the video loops).
  void restart() {
    std::lock_guard lock(mMutex);
    if (!mPlayer) return;
    JniScope jni;
    if (!jni) return;
    JNIEnv *env = jni.env;
    jclass cls = env->GetObjectClass(mPlayer);
    jmethodID seekTo = findMethod(env, cls, "seekTo", "(I)V");
    jmethodID startM = findMethod(env, cls, "start", "()V");
    if (seekTo) {
      env->CallVoidMethod(mPlayer, seekTo, static_cast<jint>(0));
      clearException(env);
    }
    if (startM) {
      env->CallVoidMethod(mPlayer, startM);
      clearException(env);
    }
    env->DeleteLocalRef(cls);
  }

  void stop() {
    std::lock_guard lock(mMutex);
    releaseLocked();
  }

private:
  std::mutex mMutex;
  jobject mPlayer = nullptr;

  void releaseLocked() {
    if (!mPlayer) return;
    JniScope jni;
    if (jni) destroyPlayer(jni.env, mPlayer);
    mPlayer = nullptr;
  }

  static void destroyPlayer(JNIEnv *env, jobject player) {
    jclass cls = env->GetObjectClass(player);
    if (jmethodID stopM = findMethod(env, cls, "stop", "()V")) {
      env->CallVoidMethod(player, stopM);
      clearException(env);
    }
    if (jmethodID releaseM = findMethod(env, cls, "release", "()V")) {
      env->CallVoidMethod(player, releaseM);
      clearException(env);
    }
    env->DeleteLocalRef(cls);
    env->DeleteGlobalRef(player);
  }
};

// ------------------------------------------------------------ saved settings
// Plain "key=value" lines in <mod config dir>/badapple_settings.txt.
struct SavedSettings {
  int blockSize = 4, fps = 0, posX = 40, posY = 80, quality = 0;
  bool loop = true, sound = true;
};

std::filesystem::path settingsPath() {
  const auto mod = pl::mod::NativeMod::current();
  if (!mod) return {};
  return std::filesystem::path(mod->getConfigDir()) / "badapple_settings.txt";
}

SavedSettings readSettings(const std::filesystem::path &path) {
  SavedSettings s;
  if (path.empty()) return s;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == kBlockSizeKey) s.blockSize = std::clamp(parseInt(value, s.blockSize), 1, 16);
    else if (key == kFpsKey) s.fps = std::clamp(parseInt(value, s.fps), 0, 30);
    else if (key == kLoopKey) s.loop = parseBool(value, s.loop);
    else if (key == kSoundKey) s.sound = parseBool(value, s.sound);
    else if (key == kPosXKey) s.posX = std::clamp(parseInt(value, s.posX), 0, 2000);
    else if (key == kPosYKey) s.posY = std::clamp(parseInt(value, s.posY), 0, 1000);
    else if (key == kQualityKey) s.quality = parseQuality(value, s.quality);
  }
  return s;
}

bool writeSettings(const std::filesystem::path &path, const SavedSettings &s) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << kBlockSizeKey << '=' << s.blockSize << '\n'
        << kFpsKey << '=' << s.fps << '\n'
        << kLoopKey << '=' << (s.loop ? "true" : "false") << '\n'
        << kSoundKey << '=' << (s.sound ? "true" : "false") << '\n'
        << kPosXKey << '=' << s.posX << '\n'
        << kPosYKey << '=' << s.posY << '\n'
        << kQualityKey << '=' << s.quality << '\n';
    if (!out) return false;
  }
  std::filesystem::rename(tmp, path, ec);  // replace the old file in one step
  return !ec;
}

struct Video {
  int w = 0, h = 0, fps = 15;
  int levels = 2;  // number of gray levels (2, 4 or 16)
  int bpp = 1;     // bits per pixel (1, 2 or 4)
  uint32_t frames = 0;
  size_t frameBytes = 0;
  std::vector<uint8_t> data;
  bool fromFile = false;

  static int bppFor(int lv) { return lv <= 2 ? 1 : (lv <= 4 ? 2 : 4); }

  // Level (0 = black ... levels-1 = white) of pixel index i = y * w + x in a frame.
  uint8_t pixel(const uint8_t *fr, size_t i) const {
    const size_t bit = i * static_cast<size_t>(bpp);
    return static_cast<uint8_t>((fr[bit >> 3] >> (8 - bpp - static_cast<int>(bit & 7))) & ((1 << bpp) - 1));
  }
  void setPixel(uint8_t *fr, size_t i, uint8_t v) const {
    const size_t bit = i * static_cast<size_t>(bpp);
    fr[bit >> 3] |= static_cast<uint8_t>(v << (8 - bpp - static_cast<int>(bit & 7)));
  }

  // Reads "BAP1" (2 levels) or "BAP2" (levels stored in the header).
  bool loadFile(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4];
    uint16_t pw = 0, ph = 0, pfps = 0, plv = 0;
    uint32_t n = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 &&
              (std::memcmp(magic, "BAP1", 4) == 0 || std::memcmp(magic, "BAP2", 4) == 0) &&
              std::fread(&pw, 2, 1, f) == 1 && std::fread(&ph, 2, 1, f) == 1 &&
              std::fread(&pfps, 2, 1, f) == 1 && std::fread(&plv, 2, 1, f) == 1 &&
              std::fread(&n, 4, 1, f) == 1 && pw > 0 && ph > 0 && pfps > 0 && n > 0;
    if (ok) {
      const int lv = std::memcmp(magic, "BAP1", 4) == 0 ? 2 : plv;
      ok = lv == 2 || lv == 4 || lv == 16;
      if (ok) {
        w = pw; h = ph; fps = pfps; frames = n; levels = lv; bpp = bppFor(lv);
        frameBytes = (static_cast<size_t>(w) * h * bpp + 7) / 8;
        data.resize(frameBytes * frames);
        ok = std::fread(data.data(), 1, data.size(), f) == data.size();
      }
    }
    std::fclose(f);
    if (!ok) clear();
    fromFile = ok;
    return ok;
  }

  // Built-in test animation (4 gray levels): bouncing box with a gray shadow.
  void makeTest() {
    w = 64; h = 48; fps = 15; frames = 120; levels = 4; bpp = 2;
    frameBytes = (static_cast<size_t>(w) * h * bpp + 7) / 8;
    data.assign(frameBytes * frames, 0);
    for (uint32_t t = 0; t < frames; ++t) {
      uint8_t *fr = &data[t * frameBytes];
      const int bx = static_cast<int>(t * 2) % (2 * (w - 12));
      const int boxX = bx < (w - 12) ? bx : 2 * (w - 12) - bx;
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          uint8_t v = 0;
          if (x == 0 || y == 0 || x == w - 1 || y == h - 1) v = 3;                  // white border
          else if (y >= 36 && y < 40) v = 1;                                         // dark gray band
          else if (y >= 40 && y < 44) v = 2;                                         // light gray band
          if (x >= boxX && x < boxX + 12 && y >= 18 && y < 30) v = 3;                // white box
          else if (x >= boxX + 2 && x < boxX + 14 && y >= 20 && y < 32) v = 1;       // gray shadow
          if (v) setPixel(fr, static_cast<size_t>(y) * w + x, v);
        }
      }
    }
    fromFile = false;
  }

  void clear() {
    data.clear(); data.shrink_to_fit();
    w = h = 0; frames = 0; frameBytes = 0;
  }
  bool loaded() const { return frames > 0; }
};

// Calls emit(x0, y0, x1, y1, level) for every rectangle (in pixels, x1/y1 exclusive)
// needed to draw one frame. Level 0 (black) is skipped: it is the background.
// Equal runs in consecutive rows are merged into one taller rectangle.
template <class Emit>
void forEachRect(const Video &v, size_t frameIdx, Emit &&emit) {
  struct Run { int x0, x1, lv, y0; };
  const uint8_t *fr = &v.data[frameIdx * v.frameBytes];
  std::vector<Run> prev, cur;
  std::vector<char> matched;
  for (int y = 0; y <= v.h; ++y) {
    cur.clear();
    if (y < v.h) {
      int x = 0;
      while (x < v.w) {
        const uint8_t lv = v.pixel(fr, static_cast<size_t>(y) * v.w + x);
        int x1 = x + 1;
        while (x1 < v.w && v.pixel(fr, static_cast<size_t>(y) * v.w + x1) == lv) ++x1;
        if (lv) cur.push_back({x, x1, lv, y});
        x = x1;
      }
    }
    matched.assign(prev.size(), 0);
    size_t i = 0;
    for (Run &c : cur) {
      while (i < prev.size() && prev[i].x0 < c.x0) ++i;
      if (i < prev.size() && prev[i].x0 == c.x0 && prev[i].x1 == c.x1 && prev[i].lv == c.lv) {
        c.y0 = prev[i].y0;
        matched[i] = 1;
      }
    }
    for (size_t k = 0; k < prev.size(); ++k) {
      if (!matched[k]) emit(prev[k].x0, prev[k].y0, prev[k].x1, y, prev[k].lv);
    }
    prev = cur;
  }
}

class BadAppleMod {
public:
  static BadAppleMod &instance() {
    static BadAppleMod mod;
    return mod;
  }

  BadAppleMod() : mSelf(*ll::mod::NativeMod::current()) {}
  [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

  bool load() {
    getSelf().getLogger().info("BadApple loaded");
    return true;
  }

  bool enable() {
    loadSaved();
    const bool ok =
        pl::modmenu::ModuleBuilder(kModuleId, "Bad Apple")
            .modId(getSelf().getId())
            .description("Plays Bad Apple as a HUD overlay. Turn this module on to start.")
            .defaultEnabled(false)
            .onToggle(onModuleToggle)
            .config(kQualityKey, "Video quality", pl::modmenu::ConfigType::Radio,
                    std::to_string(mQuality.load()), "High 480x360,Medium 240x180")
            .config(kBlockSizeKey, "Resolution (1-16)", pl::modmenu::ConfigType::SliderInt, std::to_string(mBlock.load()), "1", "16")
            .config(kFpsKey, "Playback FPS (0 = file default, 10-30)", pl::modmenu::ConfigType::SliderInt, std::to_string(mFps.load()), "0", "30")
            .config(kLoopKey, "Loop", pl::modmenu::ConfigType::Toggle, mLoop ? "true" : "false")
            .config(kSoundKey, "Play Sound", pl::modmenu::ConfigType::Toggle, mSound ? "true" : "false")
            .config(kPosXKey, "Position X", pl::modmenu::ConfigType::SliderInt, std::to_string(mPosX.load()), "0", "2000")
            .config(kPosYKey, "Position Y", pl::modmenu::ConfigType::SliderInt, std::to_string(mPosY.load()), "0", "1000")
            .onConfigChanged(onConfigChanged)
            .registerModule();
    if (!ok) {
      getSelf().getLogger().error("Failed to register module {}", kModuleId);
      return false;
    }
    getSelf().getLogger().info("Registered module {}", kModuleId);
    startWorker();
    return true;
  }

  bool disable() {
    stopWorker();
    flushSettings();
    mAudio.stop();
    clearOverlay();
    pl::modmenu::unregisterModule(kModuleId);
    getSelf().getLogger().info("Disabled");
    return true;
  }

  bool unload() {
    stopWorker();
    flushSettings();
    mAudio.stop();
    pl::modmenu::unregisterModule(kModuleId);
    return true;
  }

private:
  ll::mod::NativeMod &mSelf;
  std::atomic_bool mActive{false};
  std::atomic_bool mRunning{false};
  std::atomic_int mBlock{4}, mFps{0}, mPosX{40}, mPosY{80}, mQuality{0};
  std::atomic_bool mLoop{true}, mSound{true};
  std::atomic_bool mRestart{false};
  std::atomic_bool mReloadVideo{false};  // worker thread (re)loads the picture file
  std::atomic<long long> mDirtyAtMs{0};  // 0 = nothing to save
  std::mutex mVideoMutex;
  Video mVideo;
  Audio mAudio;
  std::string mAudioPath;
  std::thread mWorker;

  static void onModuleToggle(std::string_view id, bool enabled) {
    instance().handleToggle(id, enabled);
  }
  static void onConfigChanged(std::string_view id, std::string_view key, std::string_view value) {
    instance().handleConfig(id, key, value);
  }

  static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
  }

  void loadSaved() {
    const SavedSettings s = readSettings(settingsPath());
    mBlock = s.blockSize; mFps = s.fps; mLoop = s.loop;
    mSound = s.sound; mPosX = s.posX; mPosY = s.posY; mQuality = s.quality;
  }

  void flushSettings() {
    if (mDirtyAtMs.exchange(0) == 0) return;
    SavedSettings s;
    s.blockSize = mBlock; s.fps = mFps; s.loop = mLoop;
    s.sound = mSound; s.posX = mPosX; s.posY = mPosY; s.quality = mQuality;
    if (!writeSettings(settingsPath(), s))
      getSelf().getLogger().warn("Could not save settings");
  }

  // Saves ~0.4 s after the last change, so dragging a slider does not write on every tick.
  void saveIfIdle() {
    const long long t = mDirtyAtMs.load();
    if (t != 0 && nowMs() - t >= 400) flushSettings();
  }

  void clearOverlay() {
    pl::modmenu::submitDrawCommands(kModuleId, std::span<const pl::modmenu::DrawCommand>{});
  }

  // Loads the picture file that matches the chosen quality (caller holds mVideoMutex).
  // Quality 1 prefers bad_apple_240.bap and falls back to bad_apple.bap if it is missing.
  void loadVideoLocked() {
    const std::string dir = moduleDir();
    std::vector<std::string> names;
    if (mQuality == 1) {
      names.push_back("/resources/bad_apple_240.bap");
      names.push_back("/bad_apple_240.bap");
    }
    names.push_back("/resources/bad_apple.bap");
    names.push_back("/bad_apple.bap");
    for (const std::string &rel : names) {
      if (mVideo.loadFile(dir + rel)) {
        getSelf().getLogger().info("Loaded {}{} ({}x{}, {} frames)", dir, rel,
                                   mVideo.w, mVideo.h, mVideo.frames);
        return;
      }
    }
    mVideo.makeTest();
    getSelf().getLogger().warn("bad_apple.bap not found in {}, using test animation", dir);
  }

  void handleToggle(std::string_view id, bool enabled) {
    if (id != kModuleId) return;
    if (enabled) {
      mReloadVideo = true;  // big file: loaded on the worker thread, not on the menu thread
      mAudioPath.clear();
      {
        const std::string dir = moduleDir();
        for (const char *rel : {"/resources/bad_apple.mp3", "/bad_apple.mp3"}) {
          const std::string p = dir + rel;
          if (FILE *f = std::fopen(p.c_str(), "rb")) {
            std::fclose(f);
            mAudioPath = p;
            break;
          }
        }
        if (mAudioPath.empty()) getSelf().getLogger().warn("bad_apple.mp3 not found in {}, no sound", dir);
      }
      mRestart = true;
      mActive = true;
    } else {
      mActive = false;
      mAudio.stop();
      clearOverlay();
      std::lock_guard lock(mVideoMutex);
      mVideo.clear();  // free RAM while off
    }
    getSelf().getLogger().info("Module {} {}", id, enabled ? "enabled" : "disabled");
  }

  void handleConfig(std::string_view id, std::string_view key, std::string_view value) {
    if (id != kModuleId) return;
    if (key == kBlockSizeKey) mBlock = std::clamp(parseInt(value, mBlock.load()), 1, 16);
    else if (key == kFpsKey) mFps = std::clamp(parseInt(value, mFps.load()), 0, 30);
    else if (key == kLoopKey) mLoop = parseBool(value, mLoop.load());
    else if (key == kSoundKey) {
      mSound = parseBool(value, mSound.load());
      if (!mSound) mAudio.stop();
      else if (mActive) mRestart = true;  // restart picture and sound together
    }
    else if (key == kPosXKey) mPosX = std::clamp(parseInt(value, mPosX.load()), 0, 2000);
    else if (key == kPosYKey) mPosY = std::clamp(parseInt(value, mPosY.load()), 0, 1000);
    else if (key == kQualityKey) {
      const int q = parseQuality(value, mQuality.load());
      if (q != mQuality.load()) {
        mQuality = q;
        if (mActive) {  // switch picture file and start over together with the sound
          mReloadVideo = true;
          mRestart = true;
        }
      }
    }
    else return;
    mDirtyAtMs = nowMs();
  }

  void startWorker() {
    if (mRunning.exchange(true)) return;
    mWorker = std::thread([this] { workerLoop(); });
  }

  void stopWorker() {
    if (!mRunning.exchange(false)) return;
    if (mWorker.joinable()) mWorker.join();
  }

  void workerLoop() {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    size_t prevIdx = 0;
    bool audioOn = false;
    while (mRunning) {
      int fps = 15;
      if (mActive) {
        if (mReloadVideo.exchange(false)) {
          std::lock_guard lock(mVideoMutex);
          mVideo.clear();
          loadVideoLocked();
        }
        if (mRestart.exchange(false)) {
          audioOn = mSound && !mAudioPath.empty() && mAudio.start(mAudioPath);
          start = clock::now();  // reset the picture clock right after the sound starts
          prevIdx = 0;
        }
        std::vector<pl::modmenu::DrawCommand> cmds;
        bool finished = false;
        bool wrapped = false;
        {
          std::lock_guard lock(mVideoMutex);
          if (mVideo.loaded()) {
            fps = mFps > 0 ? mFps.load() : mVideo.fps;
            fps = std::max(fps, 1);
            const double ms = std::chrono::duration<double, std::milli>(clock::now() - start).count();
            size_t idx = static_cast<size_t>(ms * fps / 1000.0);
            if (idx >= mVideo.frames) {
              if (mLoop) idx %= mVideo.frames;
              else { idx = mVideo.frames - 1; finished = true; }
            }
            wrapped = idx < prevIdx;
            prevIdx = idx;
            buildCommands(idx, cmds);
          }
        }
        if (wrapped && audioOn) mAudio.restart();
        if (!cmds.empty()) pl::modmenu::submitDrawCommands(kModuleId, cmds);
        if (finished) {
          mActive = false;
          mAudio.stop();
        }
      }
      saveIfIdle();
      std::this_thread::sleep_for(std::chrono::milliseconds(1000 / std::max(fps, 1)));
    }
  }

  void buildCommands(size_t frameIdx, std::vector<pl::modmenu::DrawCommand> &out) const {
    const int b = std::clamp(mBlock.load(), 1, 16);
    const float ox = static_cast<float>(mPosX.load());
    const float oy = static_cast<float>(mPosY.load());
    out.push_back({.type = pl::modmenu::DrawCommandType::RectFilled, .x = ox, .y = oy,
                   .w = static_cast<float>(mVideo.w * b), .h = static_cast<float>(mVideo.h * b),
                   .color = 0xFF000000u});
    const int top = mVideo.levels - 1;
    forEachRect(mVideo, frameIdx, [&](int x0, int y0, int x1, int y1, int lv) {
      const uint32_t g = static_cast<uint32_t>(lv * 255 / top);
      out.push_back({.type = pl::modmenu::DrawCommandType::RectFilled,
                     .x = ox + static_cast<float>(x0 * b), .y = oy + static_cast<float>(y0 * b),
                     .w = static_cast<float>((x1 - x0) * b), .h = static_cast<float>((y1 - y0) * b),
                     .color = 0xFF000000u | (g << 16) | (g << 8) | g});
    });
  }
};

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
  gVm = vm;
  return JNI_VERSION_1_6;
}

PL_REGISTER_MOD(BadAppleMod, BadAppleMod::instance())
