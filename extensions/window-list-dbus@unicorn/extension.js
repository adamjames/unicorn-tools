import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import { Extension } from 'resource:///org/gnome/shell/extensions/extension.js';

const IFACE_XML = `
<node>
  <interface name="org.unicorn.WindowList">
    <method name="List">
      <arg type="s" direction="out" name="json"/>
    </method>
  </interface>
</node>`;

class WindowListDBus {
    List() {
        const windows = global.get_window_actors().map(a => {
            const w = a.get_meta_window();
            const r = w.get_frame_rect();
            return {
                title: w.get_title(),
                wm_class: w.get_wm_class(),
                x: r.x,
                y: r.y,
                width: r.width,
                height: r.height,
            };
        });
        return JSON.stringify(windows);
    }
}

export default class WindowListExtension extends Extension {
    _dbus = null;
    _ownerId = null;

    enable() {
        const nodeInfo = Gio.DBusNodeInfo.new_for_xml(IFACE_XML);
        this._dbus = Gio.DBusExportedObject.wrapJSObject(
            nodeInfo.interfaces[0],
            new WindowListDBus(),
        );
        this._dbus.export(Gio.DBus.session, '/org/unicorn/WindowList');
        this._ownerId = Gio.bus_own_name(
            Gio.BusType.SESSION,
            'org.unicorn.WindowList',
            Gio.BusNameOwnerFlags.NONE,
            null, null, null,
        );
    }

    disable() {
        if (this._dbus) {
            this._dbus.unexport();
            this._dbus = null;
        }
        if (this._ownerId) {
            Gio.bus_unown_name(this._ownerId);
            this._ownerId = null;
        }
    }
}
