using System.Net.Http.Json;
using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

const string defaultUnrealUrl = "http://127.0.0.1:31010";
var unrealUrl = GetOption(args, "--url") ?? defaultUnrealUrl;
using var http = new HttpClient { BaseAddress = new Uri(unrealUrl.TrimEnd('/') + "/") };

while (Console.ReadLine() is { } line)
{
    if (string.IsNullOrWhiteSpace(line))
    {
        continue;
    }

    try
    {
        var request = JsonNode.Parse(line)?.AsObject();
        if (request is null)
        {
            continue;
        }

        var method = request["method"]?.GetValue<string>();
        var id = request["id"]?.DeepClone();

        if (method is "notifications/initialized" or "notifications/cancelled")
        {
            continue;
        }

        JsonNode? result;
        switch (method)
        {
            case "initialize":
                result = new JsonObject
                {
                    ["protocolVersion"] = "2024-11-05",
                    ["capabilities"] = new JsonObject { ["tools"] = new JsonObject() },
                    ["serverInfo"] = new JsonObject
                    {
                        ["name"] = "robotprojetgame-unreal",
                        ["version"] = "0.1.0"
                    }
                };
                break;

            case "tools/list":
                result = ToolsList();
                break;

            case "tools/call":
                result = await CallToolAsync(request["params"]?.AsObject());
                break;

            default:
                WriteError(id, -32601, $"Method not found: {method}");
                continue;
        }

        WriteResult(id, result);
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"CodexBridge MCP error: {ex.Message}");
        WriteError(null, -32603, ex.Message);
    }
}

static string? GetOption(string[] arguments, string option)
{
    var index = Array.IndexOf(arguments, option);
    return index >= 0 && index + 1 < arguments.Length ? arguments[index + 1] : null;
}

static JsonObject ToolsList() => new()
{
    ["tools"] = new JsonArray
    {
        Tool("unreal_health", "Check that the Unreal Editor CodexBridge is running.", new JsonObject()),
        Tool("inspect_blueprint", "Inspect the graphs, nodes, pins and links of a Blueprint asset.", AssetSchema()),
        Tool("compile_blueprint", "Compile a Blueprint without saving it and return error/warning counts.", AssetSchema()),
        Tool("add_blueprint_node", "Add a supported Blueprint node. Requires explicit confirm=true and is blocked during PIE/simulation.", AddNodeSchema()),
        Tool("connect_blueprint_pins", "Connect two Blueprint pins. Requires explicit confirm=true and is blocked during PIE/simulation.", ConnectPinsSchema()),
        Tool("disconnect_blueprint_pin", "Break all links from a Blueprint pin. Requires explicit confirm=true and is blocked during PIE/simulation.", DisconnectPinSchema()),
        Tool("set_blueprint_pin_default", "Set an input pin default value. Requires explicit confirm=true and is blocked during PIE/simulation.", SetPinDefaultSchema()),
        Tool("delete_blueprint_node", "Delete a Blueprint node. Requires explicit confirm=true and is blocked during PIE/simulation.", DeleteNodeSchema()),
        Tool("save_blueprint", "Save a Blueprint package to disk. Requires explicit confirm=true and is blocked during PIE/simulation.", SaveSchema())
    }
};

static JsonObject Tool(string name, string description, JsonObject inputSchema) => new()
{
    ["name"] = name,
    ["description"] = description,
    ["inputSchema"] = inputSchema
};

static JsonObject AssetSchema() => new()
{
    ["type"] = "object",
    ["properties"] = new JsonObject
    {
        ["asset_path"] = new JsonObject
        {
            ["type"] = "string",
            ["description"] = "Unreal object path, for example /Game/Hibiki/01_Blueprints/PC_Combat.PC_Combat"
        }
    },
    ["required"] = new JsonArray("asset_path")
};

static JsonObject BlueprintWriteSchema(JsonObject extraProperties, params string[] requiredFields)
{
    var properties = new JsonObject
    {
        ["asset_path"] = new JsonObject
        {
            ["type"] = "string",
            ["description"] = "Unreal object path, for example /Game/Hibiki/01_Blueprints/PC_Combat.PC_Combat"
        },
        ["confirm"] = new JsonObject
        {
            ["type"] = "boolean",
            ["description"] = "Must be true to authorize this write operation."
        }
    };

    foreach (var property in extraProperties)
    {
        properties[property.Key] = property.Value?.DeepClone();
    }

    var required = new JsonArray("asset_path", "confirm");
    foreach (var field in requiredFields)
    {
        required.Add(field);
    }

    return new JsonObject
    {
        ["type"] = "object",
        ["properties"] = properties,
        ["required"] = required
    };
}

static JsonObject AddNodeSchema() => BlueprintWriteSchema(new JsonObject
{
    ["graph_path"] = new JsonObject { ["type"] = "string" },
    ["graph_name"] = new JsonObject { ["type"] = "string" },
    ["node_type"] = new JsonObject { ["type"] = "string", ["enum"] = new JsonArray("branch", "delay", "call_function", "variable_get", "variable_set", "custom_event", "reroute") },
    ["x"] = new JsonObject { ["type"] = "integer" },
    ["y"] = new JsonObject { ["type"] = "integer" },
    ["function_name"] = new JsonObject { ["type"] = "string" },
    ["owner_class"] = new JsonObject { ["type"] = "string" },
    ["variable_name"] = new JsonObject { ["type"] = "string" },
    ["event_name"] = new JsonObject { ["type"] = "string" },
    ["comment"] = new JsonObject { ["type"] = "string" }
}, "node_type");

static JsonObject ConnectPinsSchema() => BlueprintWriteSchema(new JsonObject
{
    ["graph_path"] = new JsonObject { ["type"] = "string" },
    ["graph_name"] = new JsonObject { ["type"] = "string" },
    ["source_node_id"] = new JsonObject { ["type"] = "string" },
    ["source_pin"] = new JsonObject { ["type"] = "string" },
    ["target_node_id"] = new JsonObject { ["type"] = "string" },
    ["target_pin"] = new JsonObject { ["type"] = "string" }
}, "source_node_id", "source_pin", "target_node_id", "target_pin");

static JsonObject DisconnectPinSchema() => BlueprintWriteSchema(new JsonObject
{
    ["graph_path"] = new JsonObject { ["type"] = "string" },
    ["graph_name"] = new JsonObject { ["type"] = "string" },
    ["node_id"] = new JsonObject { ["type"] = "string" },
    ["pin_name"] = new JsonObject { ["type"] = "string" }
}, "node_id", "pin_name");

static JsonObject SetPinDefaultSchema() => BlueprintWriteSchema(new JsonObject
{
    ["graph_path"] = new JsonObject { ["type"] = "string" },
    ["graph_name"] = new JsonObject { ["type"] = "string" },
    ["node_id"] = new JsonObject { ["type"] = "string" },
    ["pin_name"] = new JsonObject { ["type"] = "string" },
    ["value"] = new JsonObject { ["type"] = "string" }
}, "node_id", "pin_name", "value");

static JsonObject DeleteNodeSchema() => BlueprintWriteSchema(new JsonObject
{
    ["graph_path"] = new JsonObject { ["type"] = "string" },
    ["graph_name"] = new JsonObject { ["type"] = "string" },
    ["node_id"] = new JsonObject { ["type"] = "string" }
}, "node_id");

static JsonObject SaveSchema() => BlueprintWriteSchema(new JsonObject());

async Task<JsonObject> CallToolAsync(JsonObject? parameters)
{
    var name = parameters?["name"]?.GetValue<string>() ?? string.Empty;
    var arguments = parameters?["arguments"]?.AsObject() ?? new JsonObject();

    return name switch
    {
        "unreal_health" => await GetUnrealAsync("codex/health"),
        "inspect_blueprint" => await PostUnrealAsync("codex/blueprint/inspect", arguments),
        "compile_blueprint" => await PostUnrealAsync("codex/blueprint/compile", arguments),
        "add_blueprint_node" => await PostUnrealAsync("codex/blueprint/add_node", arguments),
        "connect_blueprint_pins" => await PostUnrealAsync("codex/blueprint/connect_pins", arguments),
        "disconnect_blueprint_pin" => await PostUnrealAsync("codex/blueprint/disconnect_pin", arguments),
        "set_blueprint_pin_default" => await PostUnrealAsync("codex/blueprint/set_pin_default", arguments),
        "delete_blueprint_node" => await PostUnrealAsync("codex/blueprint/delete_node", arguments),
        "save_blueprint" => await PostUnrealAsync("codex/blueprint/save", arguments),
        _ => ToolError($"Unknown tool: {name}")
    };
}

async Task<JsonObject> GetUnrealAsync(string path)
{
    using var response = await http.GetAsync(path);
    return await ToToolResultAsync(response);
}

async Task<JsonObject> PostUnrealAsync(string path, JsonObject body)
{
    var json = body.ToJsonString();
    var bytes = Encoding.UTF8.GetBytes(json);

    using var content = new ByteArrayContent(bytes);
    content.Headers.ContentType = new MediaTypeHeaderValue("application/json");
    content.Headers.ContentLength = bytes.Length;

    using var request = new HttpRequestMessage(HttpMethod.Post, path)
    {
        Content = content,
        Version = HttpVersion.Version11,
        VersionPolicy = HttpVersionPolicy.RequestVersionExact
    };
    request.Headers.TransferEncodingChunked = false;

    using var response = await http.SendAsync(request);
    return await ToToolResultAsync(response);
}

static async Task<JsonObject> ToToolResultAsync(HttpResponseMessage response)
{
    var text = await response.Content.ReadAsStringAsync();
    var isError = !response.IsSuccessStatusCode;
    return new JsonObject
    {
        ["isError"] = isError,
        ["content"] = new JsonArray
        {
            new JsonObject
            {
                ["type"] = "text",
                ["text"] = text
            }
        }
    };
}

static JsonObject ToolError(string message) => new()
{
    ["isError"] = true,
    ["content"] = new JsonArray
    {
        new JsonObject { ["type"] = "text", ["text"] = message }
    }
};

static void WriteResult(JsonNode? id, JsonNode? result)
{
    var response = new JsonObject
    {
        ["jsonrpc"] = "2.0",
        ["id"] = id,
        ["result"] = result
    };
    Console.WriteLine(response.ToJsonString());
    Console.Out.Flush();
}

static void WriteError(JsonNode? id, int code, string message)
{
    var response = new JsonObject
    {
        ["jsonrpc"] = "2.0",
        ["id"] = id,
        ["error"] = new JsonObject
        {
            ["code"] = code,
            ["message"] = message
        }
    };
    Console.WriteLine(response.ToJsonString());
    Console.Out.Flush();
}
