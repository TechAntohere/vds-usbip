param([double]$Gain = 8.0)
cd "C:\Users\Antonio\Documents\vds\build"
$env:VDS_TRANSPORT='usbip'
$env:VDS_MIC_GAIN="$Gain"
.\vdsd.exe *> C:\Users\Antonio\Documents\vds\vdsd_console.log
