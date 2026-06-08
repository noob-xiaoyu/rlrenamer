# rlrenamer 改动记录

## 缘由

实现方案 C：`--with-macros` 宏名仅在预处理器指令上下文（`#define`/`#ifdef`/`#ifndef`/`#undef`/`defined()`）中重命名，其他位置保持原样。同时修复了审查中发现的 5 个 bug。

## 修改文件清单

5 个文件，共 ~370 行改动。

---

## 1. `src/lexer.h`

| 行 | 改动 |
|----|------|
| +1 | 新增 `#include <unordered_set>` |
| +10 | 新增 `RewriteWithMacroAwareness()` 函数声明及文档注释 |

```cpp
// Macro-aware rewrite: identifiers in `macroNames` are only renamed in
// preprocessor directive positions (#define, #ifdef, #ifndef, #undef, defined()).
// Other identifiers are renamed everywhere as usual.
// When macroNames is empty, falls back to the standard RewriteWithIdentifierMap behavior.
std::string RewriteWithMacroAwareness(
    const std::string& input,
    const std::unordered_map<std::string, std::string>& renameMap,
    RewriteStats* stats,
    const std::unordered_set<std::string>& macroNames
);
```

---

## 2. `src/lexer.cpp`

### 新增：宏感知预处理器处理（~250 行）

| 函数 | 说明 |
|------|------|
| `IsSpaceChar()` | PP 行空白字符判断 |
| `ReadPreprocessorLine()` | 读取完整 PP 逻辑行（处理 `\` 续行） |
| `IsMacroNamePosition()` | 判断当前标识符位置是否为宏名位置 |
| `RewriteWithMacroAwareness()` | 主入口 |

**预处理器子循环规则**：

| 上下文 | 宏名（DefineMacro） | 非宏名标识符 |
|--------|---------------------|-------------|
| `#define NAME` | ✅ 重命名 | ✅ 重命名 |
| `#define NAME 1` | ✅ 重命名 | ✅ 重命名 |
| `#ifdef NAME` | ✅ 重命名 | ✅ 重命名 |
| `#ifndef NAME` | ✅ 重命名 | ✅ 重命名 |
| `#undef NAME` | ✅ 重命名 | ✅ 重命名 |
| `#if defined(NAME)` | ✅ 重命名 | ✅ 重命名 |
| `#define NAME(args) body` | 形参不改 | ✅ 重命名 |
| PP 指令其他位置（macro body） | ❌ 不改 | ✅ 重命名 |
| 普通代码中 | ❌ 不改 | ✅ 重命名 |

**特殊处理**：

- `defined` 关键字 → 永不重命名，下一个标识符标记为宏名位置
- `#define NAME(args)` → `(...)` 内的参数名不重命名
- 注释、字符串、字符字面量 → 在 PP 行中也正确跳过
- `macroNames` 为空时降级为原 `RewriteWithIdentifierMap`（完全向后兼容）

### 修复 #3：PP 行处理后 `atLineStart` 错误（1 行）

```diff
-            atLineStart = false;
+            atLineStart = true;  // PP line ends with \n, next char is at line start
```

### 修复 #4：行注释消耗 `\n` 后 `atLineStart` 未重置（2 行）

```diff
                 if (cc == '\n') break;
             }
+            atLineStart = true;  // line comment consumes \n, next char starts a new line
             continue;
```

---

## 3. `src/consumer_heuristics.cpp`

### 修复 #1：`prevPrevSig` 限定成员检查为空（2 行）

`obj-> volatile CloseWindow` 会被错误重命名。

```diff
-            if (prevPrevSig.kind == SigTok::Kind::Punct && ...) {
-                // e.g., whitespace between punct and ident ...
-            }
+            if (prevPrevSig.kind == SigTok::Kind::Punct && ...) {
+                // e.g. cv-qualified member access: obj->volatile Member
+                recordSuspicious(ident, "qualified", line, col, "");
+                return opt.replaceSuspicious;
+            }
```

---

## 4. `src/raylib_api_extract.cpp`

### 修复 #2：删除未调用死函数 `MergeDB`（-22 行）

消除编译器 warning C4505。该函数与 `main.cpp` 中的内联合并逻辑完全重复。

---

## 5. `src/main.cpp`

### 构建 macroNames 集合（+7 行）

```cpp
std::unordered_set<std::string> macroNames;
for (const auto& kv : symbolDb) {
    if (kv.second.kind == ApiSymbolKind::DefineMacro) {
        macroNames.insert(kv.first);
    }
}
```

### 非 Consumer 模式切换到新函数（2 行）

```diff
-            outText = RewriteWithIdentifierMap(inText, renameMap);
-            outText = RewriteWithIdentifierMap(inText, renameMap, &stats);
+            outText = RewriteWithMacroAwareness(inText, renameMap, nullptr, macroNames);
+            outText = RewriteWithMacroAwareness(inText, renameMap, &stats, macroNames);
```

---

## 6. `tests/test_basic.cpp`

新增 **9 个测试用例**（~70 行），覆盖：

| # | 测试场景 | 验证点 |
|---|---------|--------|
| 1 | `#define MACRO value` | 宏定义处重命名 |
| 2 | `#ifdef MACRO` | 条件编译处重命名 |
| 3 | `#undef MACRO` | 取消定义处重命名 |
| 4 | `#if defined(MACRO)` | defined() 内重命名 |
| 5 | `int MACRO = 0;` | 普通代码中不改 |
| 6 | `CloseWindow()` | 非宏名照常重命名 |
| 7 | 注释后的 `#define` | 注释不阻断 PP 识别 |
| 8 | 连续 PP 指令 | 全部正确处理 |
| 9 | `#define MACRO`（无值） | 无值定义也重命名 |

---

## 修复的 Bug 汇总

| # | 严重度 | 描述 | 影响 |
|---|--------|------|------|
| 1 | 🔴 Bug | `prevPrevSig` 限定成员检查空体 | `obj-> volatile Member` 错误重命名 |
| 2 | 🟡 Warning | 死函数 `MergeDB` | 编译 warning C4505 |
| 3 | 🔴 Bug | PP 行处理后 `atLineStart=false` | 连续 PP 指令除第一条外全部失效 |
| 4 | 🔴 Bug | 行注释后 `atLineStart` 未重置 | 注释后的 PP 指令不被识别 |
| 5 | 🔴 Bug | `--with-macros` 无 PP 感知 | 宏名在普通代码中被不当重命名 |

---

## 验证

- `cmake --build build --config Release` → 编译通过（仅 C4819 编码警告，不影响功能）
- `renamer_tests.exe` → 全部 11 个测试通过（2 原有 + 9 新增）
- 端到端测试 → 所有 PP 指令类型正确重命名，普通代码中宏名保持原样
