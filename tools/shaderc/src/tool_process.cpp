#include "tool_process.h"

#include "helios/core/fs.h"
#include "helios/core/process.h"

namespace helios::shaderc {

Result<ProcessResult> runProcess(const std::vector<std::string>& args, std::string_view input) {
    if (args.empty()) return Error{ErrorCode::InvalidArgument, "runProcess: no command"};
    ProcessDesc desc;
    desc.executable = fs::pathFromUtf8(args[0]);
    desc.searchPath = !desc.executable.has_parent_path();
    desc.args.assign(args.begin() + 1, args.end());
    desc.stdinMode = input.empty() ? StdioMode::Null : StdioMode::Pipe;
    desc.stdoutMode = StdioMode::Pipe;
    desc.mergeStderrIntoStdout = true;
    HELIOS_TRY_ASSIGN(ProcessOutput out, helios::runProcess(std::move(desc), input));
    ProcessResult result;
    result.status = out.exitCode;
    result.output = std::move(out.out);
    result.output += out.err;  // empty: stderr is merged into stdout
    return result;
}

} // namespace helios::shaderc
