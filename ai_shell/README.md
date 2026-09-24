# \# ai\_shell

# 

# A small Windows C project that exposes:

# 

# \- A simple shell prompt (\\`ai>\\`) with commands:

# &#x20; - \\`mem.init\\`, \\`mem.stats\\`

# &#x20; - \\`io.autotune\\`, \\`io.tune\\`

# &#x20; - \\`model.load\\`

# &#x20; - \\`engine.infer.dense\\`

# &#x20; - \\`engine.infer.dense.file\\`

# \- A basic HTTP server on \\`localhost:8080\\` with:

# &#x20; - \\`POST /infer\_dense\\` accepting JSON: `{ ""model"": ""..."", ""x"": \[ ... ] }`

# &#x20; - Returns: `{ ""y"": \[ ... ], ""time"": <seconds> }`

# 

# \## Build

# 

# 1\. Open \\`ai\_shell.sln\\` in Visual Studio 2022.

# 2\. Select x64 / Debug or Release.

# 3\. Build and run.

# 

# \## Run

# 

# \- The shell prompt appears as \\`ai>\\`.

# \- The HTTP server listens on \\`http://localhost:8080\\`.

# 

# This is a minimal educational implementation, not hardened for production.

