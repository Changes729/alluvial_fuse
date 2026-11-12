// ops.hpp — FUSE 操作表构建与全局访问
#pragma once

#include <functional>
#include <fuse.h>
#include <fuse_lowlevel.h>

#include "instance.hpp"
#include "overlay.hpp"

namespace alluvial_fuse {

class Ops : public Instance<Ops> {
private:
  friend Instance<Ops>;

  Ops(function<Overlay &()> getter) { _getOverlay = getter; }
  static function<Overlay &()> _getOverlay;

public:
  fuse_operations buildOps();

  static inline Overlay &overlay() { return _getOverlay(); }
  static int openFileCount();
};

} // namespace alluvial_fuse
