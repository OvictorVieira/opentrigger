; Inno Setup script. Build with:  ISCC /DAppVersion=0.1.0-beta.1 /DArtefacts=<path to build\OpenTrigger_artefacts\Release> OpenTrigger.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Artefacts
  #define Artefacts "..\..\build\OpenTrigger_artefacts\Release"
#endif

[Setup]
AppId={{3E1D6D52-6C5B-4F4E-9B0C-0F7A1D2B7A10}
AppName=OpenTrigger
AppVersion={#AppVersion}
AppVerName=OpenTrigger {#AppVersion}
AppPublisher=OpenTrigger
DefaultDirName={autopf}\OpenTrigger
DefaultGroupName=OpenTrigger
OutputBaseFilename=OpenTrigger-{#AppVersion}-Windows
OutputDir=output
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
DisableProgramGroupPage=yes

[Types]
Name: "full"; Description: "Plug-in and standalone app"
Name: "plugin"; Description: "VST3 plug-in only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plug-in (REAPER, Cubase, Ableton Live, Bitwig, FL Studio...)"; Types: full plugin custom; Flags: fixed
Name: "app"; Description: "Standalone app"; Types: full custom

[Files]
Source: "{#Artefacts}\VST3\OpenTrigger.vst3\*"; DestDir: "{commoncf64}\VST3\OpenTrigger.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Artefacts}\Standalone\OpenTrigger.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion

[Dirs]
Name: "{userdocs}\OpenTrigger\Library"

[Icons]
Name: "{group}\OpenTrigger"; Filename: "{app}\OpenTrigger.exe"; Components: app
Name: "{group}\OpenTrigger library folder"; Filename: "{userdocs}\OpenTrigger\Library"
