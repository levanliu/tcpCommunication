# Bazel Cpp Template

Prerequisites:
- Bazelisk
- ClangFormat
- Clang-Tidy

And this repo includes
- Bazel
- C++ with cc_library and cc_binary
- Clang-Tidy alias
- ClangFormat config file

Bazel installation
```
bazelisk
```

Build and Test
```
bazel build //...
bazel build //... --config clang-tidy
bazel test //...
```


+--------------------------------+
|        协议层 (Protocol Layer)     |
+--------------------------------+
      |
      v (请求连接)
+--------------------------------+
|     ConnectionManager          |  <-- [新增] 连接管理器 (负责连接池, 自动重连)
|     - RetryPolicy              |
+--------------------------------+
      |
      v (提供ICommunicationChannel)
+--------------------------------+
| <<interface>>                  |
| ICommunicationChannel          |  <-- 核心接口 (现在包含同步和异步方法)
|   - IMessageFramer* |  <-- [新增] 消息帧策略
+--------------------------------+
      ^
      | (实现)
+--------------------------------+
|     TcpIpChannel               |
|   - AsyncEngine (e.g. Asio)    |  <-- [新增] 异步I/O引擎
|   - TimeoutSettings            |
+--------------------------------+
