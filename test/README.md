基本测试内容：

1. 用户态 FUSE 挂载：测试用户态 FUSE 系统是否能挂载和退出。
   - 异常退出可能会导致文件系统不可用，需要脚本识别检测。
2. 基本 Overlay 覆盖测试
   - lower dir 三个文件： old、keep 和 remove
   - upper dir 两个文件： keep 和 new，其中，overlay 视图，remove 已经删除
  分别检测：
  1. old 是否存在
  2. keep 是否为最新
  3. remove 是否消失
3. 性能测试
  1. 新增文件的效率
  2. 删除文件的效率
  3. 更新文件，更新写入文件的效率
  所有效率均需要和无 FUSE 系统的耗时做对比。
4. 默认目录测试
   1. 仅传 --mount 启动时，upperdir/workdir 自动缺省到 ~/.local/<程序名>/ 下
   2. 程序需主动创建目录链（含 ~/.local/<程序名> 本身）
   3. 挂载视图写入应穿透到默认 upper 目录
   4. 显式传 upperdir/workdir 时不得触碰 ~/.local

## 脚本结构

每个测试点一个脚本，`test.sh` 为主入口，按注册表顺序依次遍历执行并汇总：

| 脚本 | 说明 |
| --- | --- |
| `test.sh` | 主入口，依次遍历各测试点，汇总套件结果 |
| `test_mount.sh` | 测试 1：用户态 FUSE 挂载与退出 |
| `test_overlay.sh` | 测试 2：基本 Overlay 覆盖语义 |
| `test_perf.sh` | 测试 3：性能对比 |
| `test_default.sh` | 测试 4：仅 --mount 时 upper/work 默认 ~/.local |
| `lib.sh` | 公共辅助（输出/PASS-FAIL 计数/挂载辅助/清理），被各脚本 source |

```bash
./test/test.sh                  # 全部测试点，按序执行
./test/test.sh overlay          # 只跑测试 2
./test/test.sh mount perf       # 只跑测试 1 和 3
./test/test.sh default          # 只跑测试 4
./test/test_perf.sh             # 单个测试点也可独立运行
PERF_N=1000 ./test/test.sh perf # 性能测试文件数
OOFS_BIN=/path/to/oofs ./test/test.sh
```