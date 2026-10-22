// Bad Apple player module (v3).
// - Registers a Mod Menu module (master switch = the module's own on/off).
// - Draws 1-bit frames as filled rectangles via pl::modmenu::submitDrawCommands.
// - If resources/bad_apple.bap is not found next to the .so, it plays a built-in
//   test animation (bouncing white box) so you can check that drawing works.
// - Audio: plays resources/bad_apple.mp3 through android.media.MediaPlayer (JNI).
//
// .bap format (little-endian):
//   "BAP1", u16 width, u16 height, u16 fps, u16 reserved, u32 frameCount,
//   then frameCount * ceil(width*height/8) bytes, row-major, MSB first, 1 = white.
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

bool parseBool(std::string_view v, bool fallback) {
  if (v == "true" || v == "1" || v == "on") return true;
  if (v == "false" || v == "0" || v == "off") return false;
  return fallback;
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

struct Video {
  int w = 0, h = 0, fps = 15;
  uint32_t frames = 0;
  size_t frameBytes = 0;
  std::vector<uint8_t> data;
  bool fromFile = false;

  bool loadFile(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4];
    uint16_t pw = 0, ph = 0, pfps = 0, rsv = 0;
    uint32_t n = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "BAP1", 4) == 0 &&
              std::fread(&pw, 2, 1, f) == 1 && std::fread(&ph, 2, 1, f) == 1 &&
              std::fread(&pfps, 2, 1, f) == 1 && std::fread(&rsv, 2, 1, f) == 1 &&
              std::fread(&n, 4, 1, f) == 1 && pw > 0 && ph > 0 && pfps > 0 && n > 0;
    if (ok) {
      w = pw; h = ph; fps = pfps; frames = n;
      frameBytes = (static_cast<size_t>(w) * h + 7) / 8;
      data.resize(frameBytes * frames);
      ok = std::fread(data.data(), 1, data.size(), f) == data.size();
    }
    std::fclose(f);
    if (!ok) clear();
    fromFile = ok;
    return ok;
  }

  // Built-in test animation: a bouncing white box with a white border.
  void makeTest() {
    w = 64; h = 48; fps = 15; frames = 120;
    frameBytes = (static_cast<size_t>(w) * h + 7) / 8;
    data.assign(frameBytes * frames, 0);
    for (uint32_t t = 0; t < frames; ++t) {
      const int bx = static_cast<int>(t * 2) % (2 * (w - 12));
      const int boxX = bx < (w - 12) ? bx : 2 * (w - 12) - bx;
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const bool border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
          const bool box = x >= boxX && x < boxX + 12 && y >= 18 && y < 30;
          if (border || box) {
            const size_t bit = static_cast<size_t>(y) * w + x;
            data[t * frameBytes + (bit >> 3)] |= static_cast<uint8_t>(1u << (7 - (bit & 7)));
          }
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
    const bool ok =
        pl::modmenu::ModuleBuilder(kModuleId, "Bad Apple")
            .modId(getSelf().getId())
            .description("Plays Bad Apple as a HUD overlay. Turn this module on to start.")
            .defaultEnabled(false)
            .onToggle(onModuleToggle)
            .config(kBlockSizeKey, "Resolution (1-16)", pl::modmenu::ConfigType::SliderInt, "4", "1", "16")
            .config(kFpsKey, "Playback FPS (0 = file default, 10-30)", pl::modmenu::ConfigType::SliderInt, "0", "0", "30")
            .config(kLoopKey, "Loop", pl::modmenu::ConfigType::Toggle, "true")
            .config(kSoundKey, "Play Sound (not implemented yet)", pl::modmenu::ConfigType::Toggle, "true")
            .config(kPosXKey, "Position X", pl::modmenu::ConfigType::SliderInt, "40", "0", "2000")
            .config(kPosYKey, "Position Y", pl::modmenu::ConfigType::SliderInt, "80", "0", "1000")
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
    mAudio.stop();
    clearOverlay();
    pl::modmenu::unregisterModule(kModuleId);
    getSelf().getLogger().info("Disabled");
    return true;
  }

  bool unload() {
    stopWorker();
    mAudio.stop();
    pl::modmenu::unregisterModule(kModuleId);
    return true;
  }

private:
  ll::mod::NativeMod &mSelf;
  std::atomic_bool mActive{false};
  std::atomic_bool mRunning{false};
  std::atomic_int mBlock{4}, mFps{0}, mPosX{40}, mPosY{80};
  std::atomic_bool mLoop{true}, mSound{true};
  std::atomic_bool mRestart{false};
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

  void clearOverlay() {
    pl::modmenu::submitDrawCommands(kModuleId, std::span<const pl::modmenu::DrawCommand>{});
  }

  void handleToggle(std::string_view id, bool enabled) {
    if (id != kModuleId) return;
    if (enabled) {
      {
        std::lock_guard lock(mVideoMutex);
        if (!mVideo.loaded()) {
          const std::string dir = moduleDir();
          bool ok = false;
          for (const char *rel : {"/resources/bad_apple.bap", "/bad_apple.bap"}) {
            if (mVideo.loadFile(dir + rel)) {
              getSelf().getLogger().info("Loaded {}{} ({}x{}, {} frames)", dir, rel,
                                         mVideo.w, mVideo.h, mVideo.frames);
              ok = true;
              break;
            }
          }
          if (!ok) {
            mVideo.makeTest();
            getSelf().getLogger().warn("bad_apple.bap not found in {}, using test animation", dir);
          }
        }
      }
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
      std::this_thread::sleep_for(std::chrono::milliseconds(1000 / std::max(fps, 1)));
    }
  }

  void buildCommands(size_t frameIdx, std::vector<pl::modmenu::DrawCommand> &out) const {
    const int b = std::clamp(mBlock.load(), 1, 16);
    const float ox = static_cast<float>(mPosX.load());
    const float oy = static_cast<float>(mPosY.load());
    const uint8_t *fr = &mVideo.data[frameIdx * mVideo.frameBytes];
    out.reserve(static_cast<size_t>(mVideo.h) * 4 + 1);
    out.push_back({.type = pl::modmenu::DrawCommandType::RectFilled, .x = ox, .y = oy,
                   .w = static_cast<float>(mVideo.w * b), .h = static_cast<float>(mVideo.h * b),
                   .color = 0xFF000000u});
    for (int y = 0; y < mVideo.h; ++y) {
      int runStart = -1;
      for (int x = 0; x <= mVideo.w; ++x) {
        bool white = false;
        if (x < mVideo.w) {
          const size_t bit = static_cast<size_t>(y) * mVideo.w + x;
          white = (fr[bit >> 3] >> (7 - (bit & 7))) & 1;
        }
        if (white && runStart < 0) runStart = x;
        if (!white && runStart >= 0) {
          out.push_back({.type = pl::modmenu::DrawCommandType::RectFilled,
                         .x = ox + static_cast<float>(runStart * b), .y = oy + static_cast<float>(y * b),
                         .w = static_cast<float>((x - runStart) * b), .h = static_cast<float>(b),
                         .color = 0xFFFFFFFFu});
          runStart = -1;
        }
      }
    }
  }
};

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
  gVm = vm;
  return JNI_VERSION_1_6;
}

PL_REGISTER_MOD(BadAppleMod, BadAppleMod::instance())
