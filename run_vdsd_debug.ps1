cd "C:\Users\Antonio\Documents\vds\build"
$env:VDS_TRANSPORT='usbip'
$env:VDS_USBIP_DEBUG='1'
.\vdsd.exe *> C:\Users\Antonio\Documents\vds\vdsd_console.log
