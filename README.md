
# ビルド方法

### trace_clientのビルド

```
cd trace_client/build
cmake -G "Visual Studio 17 2022" -A x64 -DDynamoRIO_ROOT="../../external/DynamoRIO" ..
cmake --build . --config Release
```