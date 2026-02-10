# Host-side streaming tools (cosmic_cast, mocks, etc.)
{ pkgs, unstable }:

let
  # Python with all required packages for streaming tools
  pythonEnv = unstable.python313.withPackages (ps: with ps; [
    pip
    setuptools
    pyserial
    ptyprocess
    websockets
    pillow
    pygobject3
    dbus-python
    pygame  # for cosmic_mock.py
  ]);
in
{
  buildInputs = [
    pythonEnv
    # Screen capture dependencies
    unstable.ffmpeg-full
    unstable.xdotool
    unstable.grim
    # Emulators (for game streaming) - install separately if needed
    # unstable.pcsx2
    # unstable.duckstation
    # GTK for file dialogs
    unstable.gsettings-desktop-schemas
    unstable.gtk3
    # GStreamer + PipeWire for portal-based screen capture
    unstable.gst_all_1.gstreamer
    unstable.gst_all_1.gst-plugins-base
    unstable.gst_all_1.gst-plugins-good
    unstable.pipewire
  ];

  shellHook = ''
    # GTK schemas needed by file chooser dialogs
    export XDG_DATA_DIRS="${unstable.gsettings-desktop-schemas}/share/gsettings-schemas/${unstable.gsettings-desktop-schemas.name}:${unstable.gtk3}/share/gsettings-schemas/${unstable.gtk3.name}:$XDG_DATA_DIRS"
    # GStreamer plugin path for PipeWire source
    export GST_PLUGIN_SYSTEM_PATH_1_0="${unstable.gst_all_1.gstreamer}/lib/gstreamer-1.0:${unstable.gst_all_1.gst-plugins-base}/lib/gstreamer-1.0:${unstable.gst_all_1.gst-plugins-good}/lib/gstreamer-1.0:${unstable.pipewire}/lib/gstreamer-1.0:$GST_PLUGIN_SYSTEM_PATH_1_0"
    # GObject introspection typelibs for PyGObject (Gst, GstApp, etc.)
    export GI_TYPELIB_PATH="${unstable.gst_all_1.gstreamer.out}/lib/girepository-1.0:${unstable.gst_all_1.gst-plugins-base}/lib/girepository-1.0:${unstable.glib.out}/lib/girepository-1.0:$GI_TYPELIB_PATH"
  '';
}
