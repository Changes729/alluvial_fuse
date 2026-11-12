#include "id.hpp"
#include "ops.hpp"
#include "overlay.hpp"
#include <cstring>
#include <fuse.h>
#include <fuse_lowlevel.h>
#include <getopt.h>
#include <iostream>

using namespace std;
using namespace alluvial_fuse;

using FUSEoOpt_t = vector<string>;

int main(int argc, char **argv) {
  /** local definitions -------------------------------- FOLD BEGIN */
  id.filename = [v0_path = argv[0]]() -> const char * {
    const char *base = v0_path;
    for (const char *p = v0_path; *p != '\0'; ++p) {
      if (*p == '/') {
        base = p + 1;
      }
    }

    return base;
  };
  auto get_options = [&argc, &argv]() -> tuple<OverlayConfig, FUSEoOpt_t> {
    OverlayConfig ovl_cfg;
    FUSEoOpt_t fuse_o_opts;

    while (1) {
      int option_index = 0;
      struct option long_options[] = {
          /** basic options and utils */
          {"version", no_argument, 0, 'v'},
          {"help", no_argument, 0, 'h'},

          /** required options */
          {"mount", required_argument, 0, 'm'},
          {"workdir", required_argument, 0, 0},
          {"upperdir", required_argument, 0, 0},
          {0, 0, 0, 0},
      };

      int c = getopt_long(argc, argv, "vhm:o", long_options, &option_index);
      if (c == -1) {
        break;
      }

      switch (c) {
      case 0:
        printf("option %s", long_options[option_index].name);
        if (optarg)
          printf(" with arg %s", optarg);
        printf("\n");

        if (strcmp(long_options[option_index].name, "workdir") == 0) {
          ovl_cfg.work = optarg;
        } else if (strcmp(long_options[option_index].name, "upperdir") == 0) {
          ovl_cfg.upper = optarg;
        }
        break;

      case 'v':
        printf("Alluvial FUSE version 0.1 | current name: %s\n", argv[0]);
        break;

      case 'm':
        ovl_cfg.mount = optarg;
        break;

      case 'o':
        fuse_o_opts.push_back(optarg);
        break;

      default:
        break;
      }
    }

    return {ovl_cfg, fuse_o_opts};
  };
  auto get_fuse = [](const FUSEoOpt_t &fuse_o_opts,
                     fuse_operations ops) -> struct fuse * {
    /** fuse args (libfuse arguments) */
    fuse_args fargs = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&fargs, id.filename()); // free
    {
      string all = "fsname=alluvial_fuse,subtype=alluvial_fuse,"
                   "default_permissions";
      for (const auto &o : fuse_o_opts) {
        all += "," + o;
      }
      fuse_opt_add_arg(&fargs, "-o");        // free
      fuse_opt_add_arg(&fargs, all.c_str()); // free
    }

    struct fuse *fuse = ::fuse_new(&fargs, &ops, sizeof(ops), nullptr);
    fuse_opt_free_args(&fargs);
    if (!fuse) {
      throw invalid_argument("fuse_new 失败（参数或挂载点问题）");
    }

    return fuse;
  };
  auto get_session = [](struct fuse *fuse,
                        const char *mount_path) -> struct fuse_session * {
    /** fuse sessions */
    struct fuse_session *se = ::fuse_get_session(fuse);
    if (::fuse_set_signal_handlers(se) != 0) {
      throw runtime_error("fuse_set_signal_handlers 失败");
    }

    if (::fuse_session_mount(se, mount_path) != 0) {
      ::fuse_destroy(fuse);
      throw runtime_error("fuse_session_mount 失败");
    }
    std::printf("[oofs] 已挂载 %s\n", mount_path);
    return se;
  };
  /** local definitions ---------------------------------- FOLD END */

  cout << "current name :" << id.filename() << endl;
  try {
    Overlay overlay;
    auto [cfg, fuse_o_opts] = get_options();
    cfg.weakly_canonical();
    overlay.init(move(cfg)); /** open dir first. */
    Ops::create([&]() -> Overlay & { return overlay; });

    auto fuse = get_fuse(fuse_o_opts, Ops::instance().buildOps());
    auto se = get_session(fuse, overlay.config().mount.c_str());

    int rc = ::fuse_loop(fuse);

    ::fuse_session_unmount(se);
    ::fuse_remove_signal_handlers(se);
    ::fuse_destroy(fuse);

    std::printf("[oofs] 已退出 (rc=%d)\n", rc);
  } catch (invalid_argument &e) {
    cout << "Error: " << e.what() << endl;
    return 1;
  } catch (runtime_error &e) {
    cout << "Error: " << e.what() << endl;
    return 1;
  }

  return 0;
}
