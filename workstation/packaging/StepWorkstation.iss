#ifndef StageDir
  #error Specify /DStageDir=<prepared runtime folder>
#endif
#ifndef ReleaseDir
  #define ReleaseDir "..\releases"
#endif
[Setup]
AppId={{B32FBE09-AD8A-48A5-A4A8-78C9F54DD68B}
AppName=STEP Print Workstation
AppVersion=2026.11.14
AppPublisher=STEPROBOTICS
DefaultDirName={localappdata}\Programs\STEP Print Workstation
DefaultGroupName=STEP Print Workstation
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir={#ReleaseDir}
OutputBaseFilename=StepWorkstation-2026.11.14-win64-Setup
UninstallDisplayIcon={app}\pc_control\native\StepWorkstation.exe
SetupLogging=yes
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{group}\STEP Print Workstation"; Filename: "{app}\pc_control\native\StepWorkstation.exe"; WorkingDir: "{app}"
Name: "{group}\Setup instructions"; Filename: "{app}\README.md"
Name: "{autodesktop}\STEP Print Workstation"; Filename: "{app}\pc_control\native\StepWorkstation.exe"; Tasks: desktopicon; WorkingDir: "{app}"
[Run]
Filename: "{app}\pc_control\native\StepWorkstation.exe"; Description: "Launch STEP Print Workstation"; Flags: nowait postinstall skipifsilent
