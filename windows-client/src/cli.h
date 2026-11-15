// FlowCast Windows client - the console front end.
//
// Kept as a separate entry point because the shipped binary is a GUI-subsystem
// exe: wWinMain decides between the window and this, and attaches a console
// only when it actually needs stdout.
#pragma once

namespace fc {

int cli_main(int argc, char** argv);

// Attaches to the launching console, or allocates one. Also rebinds
// stdout/stderr, which a /SUBSYSTEM:WINDOWS binary does not have by default.
void cli_attach_console();

} // namespace fc
