#include <misuc/engine/fx/Lv2Host.h>

#include <cstring>

#ifdef MISUC_LV2
#include <lilv/lilv.h>

#include <map>
#include <utility>

namespace {

// The concrete instance behind the opaque handle. Everything a plugin render
// needs sits here; it is allocated on the control thread and only ever READ by
// the audio thread while a publish is live, so runInstance() performs no
// allocation and touches no lilv world state.
struct FxInstance {
  LilvInstance* inst = nullptr;
  uint32_t nIn = 0;
  uint32_t nOut = 0;
  float* inL = nullptr;
  float* inR = nullptr;
  float* outL = nullptr;
  float* outR = nullptr;
  float* ctrlIn = nullptr;  // control/cv input targets (also the port buffers)
  float* ctrlOut = nullptr; // control output sinks the plugin may write
  uint32_t nCtrlIn = 0;
  uint32_t nCtrlOut = 0;
  // port index -> ctrlIn slot (UINT32_MAX = not an automatable control input).
  // Sized to the plugin's port count once at instantiate time; only ever read
  // by the audio thread through setControl().
  std::vector<uint32_t> ctrlSlotByPort;
};

LilvNode* builtin(LilvWorld* w, const char* uri) {
  return lilv_new_uri(w, uri);
}

} // namespace

#endif // MISUC_LV2

namespace misuc::engine {

struct Lv2Host::Impl {
#ifdef MISUC_LV2
  LilvWorld* world = nullptr;
#endif
  bool scanned = false;
  std::vector<FxPluginInfo> catalog;
  // Control-input port hints for instantiation (port index -> param info).
  std::map<std::string, const FxPluginInfo*> byUri;
};

Lv2Host::Lv2Host() : impl_(new Impl) {
#ifdef MISUC_LV2
  impl_->world = lilv_world_new();
  if (impl_->world)
    lilv_world_load_all(impl_->world);
#endif
}

Lv2Host::~Lv2Host() {
#ifdef MISUC_LV2
  if (impl_->world) {
    // The world must outlive the instances we created from it: free any
    // catalog is just metadata (no instances owned here), the world owns its
    // nodes. Instances live in the engine slots and are freed separately.
    lilv_world_free(impl_->world);
  }
#endif
  delete impl_;
}

bool Lv2Host::available() const noexcept {
#ifdef MISUC_LV2
  return impl_ && impl_->world != nullptr;
#else
  return false;
#endif
}

std::vector<FxPluginInfo> Lv2Host::discoverEffects() {
  std::vector<FxPluginInfo> out;
#ifdef MISUC_LV2
  if (impl_->scanned) {
    out = impl_->catalog;
    return out;
  }
  if (!impl_->world)
    return out;

  const LilvPlugins* plugins = lilv_world_get_all_plugins(impl_->world);
  if (!plugins)
    return out;

  LilvNode* inClass = builtin(impl_->world, LILV_URI_INPUT_PORT);
  LilvNode* outClass = builtin(impl_->world, LILV_URI_OUTPUT_PORT);
  LilvNode* audioClass = builtin(impl_->world, LILV_URI_AUDIO_PORT);
  LilvNode* controlClass = builtin(impl_->world, LILV_URI_CONTROL_PORT);
  LilvNode* cvClass = builtin(impl_->world, LILV_URI_CV_PORT);

  LILV_FOREACH(plugins, p, plugins) {
    const LilvPlugin* plugin = lilv_plugins_get(plugins, p);
    if (!plugin)
      continue;
    const char* uri = lilv_node_as_string(lilv_plugin_get_uri(plugin));
    if (!uri)
      continue;

    FxPluginInfo info;
    info.uri = uri;

    if (const LilvNode* n = lilv_plugin_get_name(plugin))
      if (const char* s = lilv_node_as_string(n))
        info.name = s;
    if (const LilvNode* n = lilv_plugin_get_author_name(plugin))
      if (const char* s = lilv_node_as_string(n))
        info.author = s;

    uint32_t nAudioIn = 0, nAudioOut = 0, nCtrlIn = 0, nCtrlOut = 0;
    bool hostable = true;
    const uint32_t nPorts = lilv_plugin_get_num_ports(plugin);
    for (uint32_t i = 0; i < nPorts && hostable; ++i) {
      const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
      const bool isIn = lilv_port_is_a(plugin, port, inClass);
      const bool isAudio = lilv_port_is_a(plugin, port, audioClass);
      const bool isControl = lilv_port_is_a(plugin, port, controlClass);
      const bool isCv = lilv_port_is_a(plugin, port, cvClass);
      if (!isIn && !lilv_port_is_a(plugin, port, outClass)) {
        hostable = false; // directionless port: not connectable
        break;
      }
      if (!isAudio && !isControl && !isCv) {
        hostable = false; // event/atom/… ports: unsupported for the flat chain
        break;
      }
      if (isAudio) {
        if (isIn)
          ++nAudioIn;
        else
          ++nAudioOut;
      } else if (isIn) {
        ++nCtrlIn;
        const uint32_t idx = lilv_port_get_index(plugin, port);
        FxParamInfo pi;
        pi.index = idx;
        if (const LilvNode* n = lilv_port_get_name(plugin, port))
          if (const char* s = lilv_node_as_string(n))
            pi.label = s;
        LilvNode *defNode = nullptr, *minNode = nullptr, *maxNode = nullptr;
        lilv_port_get_range(plugin, port, &defNode, &minNode, &maxNode);
        if (minNode && lilv_node_is_float(minNode))
          pi.min = static_cast<float>(lilv_node_as_float(minNode));
        if (maxNode && lilv_node_is_float(maxNode))
          pi.max = static_cast<float>(lilv_node_as_float(maxNode));
        if (defNode && lilv_node_is_float(defNode))
          pi.def = static_cast<float>(lilv_node_as_float(defNode));
        if (defNode)
          lilv_node_free(defNode);
        if (minNode)
          lilv_node_free(minNode);
        if (maxNode)
          lilv_node_free(maxNode);
        info.params.push_back(std::move(pi));
      } else {
        ++nCtrlOut;
      }
    }

    if (hostable && nAudioIn > 0 && nAudioOut > 0) {
      info.audioIns = nAudioIn;
      info.audioOuts = nAudioOut;
      info.ctrlOuts = nCtrlOut;
      impl_->catalog.push_back(std::move(info));
    }
  }

  // The world owns the plugin collection (no lilv_plugins_free in this API);
  // only the class/URI nodes we created are freed below.
  lilv_node_free(inClass);
  lilv_node_free(outClass);
  lilv_node_free(audioClass);
  lilv_node_free(controlClass);
  lilv_node_free(cvClass);

  impl_->byUri.clear();
  for (const FxPluginInfo& p : impl_->catalog)
    impl_->byUri.emplace(p.uri, &p);
  impl_->scanned = true;
  out = impl_->catalog;
#else
  (void)out;
#endif
  return out;
}

const FxPluginInfo* Lv2Host::find(const std::string& uri) const {
#ifdef MISUC_LV2
  auto it = impl_->byUri.find(uri);
  return it != impl_->byUri.end() ? it->second : nullptr;
#else
  (void)uri;
  return nullptr;
#endif
}

void* Lv2Host::instantiate(const std::string& uri,
                           const std::vector<FxParam>& defaults,
                           float sampleRate, uint32_t maxFrames) {
#ifdef MISUC_LV2
  if (!impl_->world || maxFrames == 0)
    return nullptr;
  if (!impl_->scanned)
    discoverEffects();
  const FxPluginInfo* info = find(uri);
  if (!info)
    return nullptr;

  LilvNode* uriNode = builtin(impl_->world, uri.c_str());
  const LilvPlugin* plugin =
      lilv_plugins_get_by_uri(lilv_world_get_all_plugins(impl_->world), uriNode);
  lilv_node_free(uriNode);
  if (!plugin)
    return nullptr;

  const uint32_t nAin = info->audioIns;
  const uint32_t nAout = info->audioOuts;
  FxInstance* f = new FxInstance;
  f->inst = lilv_plugin_instantiate(plugin, sampleRate, nullptr);
  if (!f->inst) {
    delete f;
    return nullptr;
  }
  f->nIn = nAin;
  f->nOut = nAout;
  f->inL = new float[maxFrames];
  f->inR = new float[maxFrames];
  f->outL = new float[maxFrames];
  f->outR = new float[maxFrames];
  f->ctrlIn = new float[info->params.size()];
  f->ctrlOut = new float[info->ctrlOuts ? info->ctrlOuts : 1];
  f->nCtrlIn = static_cast<uint32_t>(info->params.size());
  f->nCtrlOut = info->ctrlOuts;

  // Map session-saved values over the plugin defaults (port-indexed).
  std::map<uint32_t, float> overrides;
  for (const FxParam& p : defaults)
    overrides[p.index] = p.value;

  uint32_t k = 0;      // control input slot
  uint32_t ck = 0;     // control output sink slot
  uint32_t aIn = 0;    // audio input channel
  uint32_t aOut = 0;   // audio output channel
  std::fill(f->ctrlIn, f->ctrlIn + info->params.size(), 0.f);
  std::fill(f->ctrlOut, f->ctrlOut + (info->ctrlOuts ? info->ctrlOuts : 1),
            0.f);

  LilvNode* inClass = builtin(impl_->world, LILV_URI_INPUT_PORT);
  LilvNode* outClass = builtin(impl_->world, LILV_URI_OUTPUT_PORT);
  LilvNode* audioClass = builtin(impl_->world, LILV_URI_AUDIO_PORT);

  const uint32_t nPorts = lilv_plugin_get_num_ports(plugin);
  f->ctrlSlotByPort.assign(nPorts, UINT32_MAX);
  for (uint32_t i = 0; i < nPorts; ++i) {
    const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
    const uint32_t idx = lilv_port_get_index(plugin, port);
    const bool isIn = lilv_port_is_a(plugin, port, inClass);
    const bool isAudio = lilv_port_is_a(plugin, port, audioClass);
    if (isAudio) {
      float* buf = isIn ? (aIn++ == 0 ? f->inL : f->inR)
                        : (aOut++ == 0 ? f->outL : f->outR);
      lilv_instance_connect_port(f->inst, idx, buf);
      continue;
    }
    if (isIn) {
      float def = 0.f;
      for (const FxParamInfo& pi : info->params)
        if (pi.index == idx) {
          def = pi.def;
          break;
        }
      auto it = overrides.find(idx);
      if (it != overrides.end())
        def = it->second;
      f->ctrlIn[k] = def;
      lilv_instance_connect_port(f->inst, idx, &f->ctrlIn[k]);
      if (idx < f->ctrlSlotByPort.size())
        f->ctrlSlotByPort[idx] = k; // maps this control port to its ctrlIn slot
      ++k;
    } else {
      lilv_instance_connect_port(f->inst, idx, &f->ctrlOut[ck]);
      ++ck;
    }
  }

  lilv_node_free(inClass);
  lilv_node_free(outClass);
  lilv_node_free(audioClass);
  return f;
#else
  (void)uri;
  (void)defaults;
  (void)sampleRate;
  (void)maxFrames;
  return nullptr;
#endif
}

void Lv2Host::freeInstance(void* handle) {
#ifdef MISUC_LV2
  FxInstance* f = static_cast<FxInstance*>(handle);
  if (!f)
    return;
  delete[] f->inL;
  delete[] f->inR;
  delete[] f->outL;
  delete[] f->outR;
  delete[] f->ctrlIn;
  delete[] f->ctrlOut;
  if (f->inst)
    lilv_instance_free(f->inst);
  delete f;
#else
  (void)handle;
#endif
}

void Lv2Host::run(void* handle, const float* inL, const float* inR,
                  float* outL, float* outR, uint32_t frames, bool enabled) {
#ifdef MISUC_LV2
  FxInstance* f = static_cast<FxInstance*>(handle);
  if (!f || frames == 0)
    return;
  if (!enabled || !f->inst) {
    // Bypass: short-circuit the audio unchanged (used by the UI enable toggle
    // keep the chain topology, plus safe as a defensive path).
    std::memcpy(outL, inL, sizeof(float) * frames);
    if (f->nOut >= 2)
      std::memcpy(outR, inR, sizeof(float) * frames);
    else
      std::memcpy(outR, inL, sizeof(float) * frames);
    return;
  }
  std::memcpy(f->inL, inL, sizeof(float) * frames);
  if (f->nIn >= 2)
    std::memcpy(f->inR, inR, sizeof(float) * frames);
  else
    std::memcpy(f->inR, f->inL, sizeof(float) * frames); // mono: L into both
  std::fill_n(f->ctrlOut, f->nCtrlOut, 0.f);
  lilv_instance_run(f->inst, frames);
  std::memcpy(outL, f->outL, sizeof(float) * frames);
  if (f->nOut >= 2)
    std::memcpy(outR, f->outR, sizeof(float) * frames);
  else
    std::memcpy(outR, f->outL, sizeof(float) * frames);
#else
  (void)handle;
  (void)inL;
  (void)inR;
  (void)outL;
  (void)outR;
  (void)frames;
  (void)enabled;
#endif
}

void Lv2Host::setControl(void* handle, uint32_t portIndex, float value) {
#ifdef MISUC_LV2
  FxInstance* f = static_cast<FxInstance*>(handle);
  if (!f)
    return;
  if (portIndex >= f->ctrlSlotByPort.size())
    return;
  const uint32_t slot = f->ctrlSlotByPort[portIndex];
  if (slot == UINT32_MAX || slot >= f->nCtrlIn)
    return;
  f->ctrlIn[slot] = value; // the plugin reads this buffer at its next run()
#else
  (void)handle;
  (void)portIndex;
  (void)value;
#endif
}

} // namespace misuc::engine