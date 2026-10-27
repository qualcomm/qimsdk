#!/usr/bin/env python3

################################################################################
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
################################################################################

import os
import gi
import threading
import subprocess
import urllib.request
import signal

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gio, Gdk, GdkPixbuf, GLib

# Define SocID for v66 architecture
V66_SOC_IDS = {
    "kTALOS_QCS610": 401,
    "kTALOS_QCS410": 406,
    "kTALOS_QCS615": 680,
    "CQ2390M": 756,
    "CQ2390S": 758,
    "IQ2390S": 759,
}

# Mapping of applications to their required models and labels
APP_REQUIREMENTS = {
    "Classification": {
        "models": ["inception_v3_quantized.tflite"],
        "labels": ["classification.json"],
    },
    "Face Detection": {
        "models": ["face_det_lite_quantized.tflite"],
        "labels": ["face_detection.json"],
    },
    "Pose Detection": {
        "models": ["hrnet_pose_quantized.tflite"],
        "labels": ["hrnet_pose.json", "hrnet_pose_settings.json"],
    },
    "Gesture Recognition": {
        "models": [
            "palm_detection.tflite",
            "hand_landmark.tflite",
            "gesture_embedder.tflite",
            "canned_gesture_classifier.tflite",
        ],
        "labels": [
            "palmd_labels.json",
            "palmd_settings.json",
            "hlandmark_labels.json",
            "hlandmark_settings.json",
            "gesture_labels.json",
        ],
    },
    "ObjectDetection": {
        "models": ["yolox_quantized.tflite"],
        "labels": ["yolox.json"],
    },
    "Parallel-AI-Fusion": {
        "models": [
            "yolox_quantized.tflite",
            "inception_v3_quantized.tflite",
            "hrnet_pose_quantized.tflite",
            "deeplabv3_plus_mobilenet_quantized.tflite",
        ],
        "labels": [
            "yolox.json",
            "classification.json",
            "hrnet_pose.json",
            "hrnet_pose_settings.json",
            "deeplabv3_resnet50.json",
        ],
    },
    "Daisychain Pose": {
        "models": ["yolox_quantized.tflite", "hrnet_pose_quantized.tflite"],
        "labels": ["yolox.json", "hrnet_pose.json", "hrnet_pose_settings.json"],
    },
    "Multistream": {
        "models": ["yolox_quantized.tflite"],
        "labels": ["yolox.json"],
    },
}

# Common media file needed for file source
COMMON_MEDIA = ["video.mp4"]

NON_AI_APPLICATIONS = [
    "Record live video",
    "DashCamera",
    "VideoWall",
    "VideoTestSrc",
]

V66_AI_APPLICATIONS = [
    "Classification",
    "Face Detection",
    "Pose Detection",
    "Gesture Recognition",
]

NON_V66_AI_APPLICATIONS = [
    "ObjectDetection",
    "Parallel-AI-Fusion",
    "Face Detection",
    "Daisychain Pose",
    "Multistream",
    "Gesture Recognition",
]

AI_APPLICATIONS = set(V66_AI_APPLICATIONS) | set(NON_V66_AI_APPLICATIONS)


# Define the base directory for all qdemo artifacts under $HOME/qdemo
QDEMO_HOME = os.path.join(os.path.expanduser("~"), "qdemo")
QDEMO_MODELS_DIR = os.path.join(QDEMO_HOME, "models")
QDEMO_LABELS_DIR = os.path.join(QDEMO_HOME, "labels")
QDEMO_MEDIA_DIR = os.path.join(QDEMO_HOME, "media")

# Maximum number of /dev/videoN nodes to probe when searching for a USB camera.
MAX_VIDEO_DEVICES = 32
SOC_DEV_PATH_PRIMARY = "/sys/devices/soc0/soc_id"
SOC_DEV_PATH_SECONDARY = "/sys/devices/system/soc/soc0/id"
CAMX_SOCKET_PATH = "/run/cam_server/le_cam_socket"

def setup_qdemo_dirs():
    """Create the qdemo directory structure under $HOME if it doesn't exist."""
    os.makedirs(QDEMO_MODELS_DIR, exist_ok=True)
    os.makedirs(QDEMO_LABELS_DIR, exist_ok=True)
    os.makedirs(QDEMO_MEDIA_DIR, exist_ok=True)

class DemoWindow(Gtk.Window):
    def __init__(self):
        """Create Demo window"""
        super().__init__()

        self.process = None

        # Ensure qdemo directories exist
        setup_qdemo_dirs()

        # Initialize attributes
        self.popup = None
        self.wifi_popup = None
        self.check_download = None
        self.download_artifacts = False
        self.ssid = None

        # Fetch Soc ID of the device
        self.soc_id = self.get_soc_id()
        self.v66_arch = self.is_v66_arch(self.soc_id)

        # Check Wi-Fi status every 2 seconds
        self.label = Gtk.Label(label="Wi-Fi status")
        GLib.timeout_add(2000, self.check_wifi_status)

        # Create labels for displaying status, SSID, and IP address
        self.status = Gtk.Label(label="Checking.....")
        self.ssid_label = Gtk.Label(label="SSID:")
        self.ssid_value = Gtk.Label(label="Fetching...")
        self.ip_label = Gtk.Label(label="IP Address:")
        self.ip_value = Gtk.Label(label="Fetching...")

        # Create a Grid with specified column and row spacing
        grid = Gtk.Grid(column_spacing=10, row_spacing=10)
        grid.set_margin_end(20)
        grid.set_margin_start(20)

        # Create a HeaderBar and set its title and subtitle
        header = Gtk.HeaderBar()
        header.set_title("Experience Application Demo")
        header.set_subtitle("Quick reference Demo")
        self.set_titlebar(header)

        # Load the CSS data
        self.css_provider = Gtk.CssProvider()
        self.css_provider.load_from_data(b"""
            headerbar {
                background-color: #2196F3;
                color: blue;
            }
        """)

        # Apply the CSS to the default screen
        screen = Gdk.Screen.get_default()
        style_context = Gtk.StyleContext()
        style_context.add_provider_for_screen(screen, self.css_provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        # Create a button with the label "Exit"
        self.exit_app = Gtk.Button(label="Exit")

        # Connect the button to the callback function
        self.exit_app.connect("clicked", self.on_exit_button_clicked)

        # Create a menu bar
        menubar = Gtk.MenuBar()

        # Wi-Fi menu
        wifimenu = Gtk.Menu()
        wifi_item = Gtk.MenuItem(label="Wi-Fi")
        wifi_item.set_submenu(wifimenu)
        menubar.append(wifi_item)

        helpmenu = Gtk.Menu()
        # Add a help item
        help_item = Gtk.MenuItem(label="Help")
        help_item.set_submenu(helpmenu)
        menubar.append(help_item)

        wifi_connect = Gtk.MenuItem(label="Connect")
        wifi_connect.connect("activate", self.establish_connection)
        wifimenu.append(wifi_connect)

        wifi_disconnect = Gtk.MenuItem(label="Disconnect")
        wifi_disconnect.connect("activate", self.disconnect_from_wifi)
        wifimenu.append(wifi_disconnect)

        help_content = Gtk.MenuItem(label="About")
        help_content.connect("activate", self.show_help)
        helpmenu.append(help_content)

        # Create a box to pack widgets
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        vbox.pack_start(menubar, False, False, 0)
        vbox.pack_start(grid, True, True, 0)
        self.add(vbox)

        # select the source
        self.src_label = Gtk.Label(label="Source")
        sources = [
            "On-Device-Camera",
            "USB-Camera",
        ]
        self.src_select = Gtk.ComboBoxText()
        for src in sources:
            self.src_select.append_text(src)

        # Set the default active application
        self.src_select.set_active(0)

        # select the application
        self.app_label = Gtk.Label(label="Applications")
        if self.v66_arch:
            architecture_applications = V66_AI_APPLICATIONS
        else:
            architecture_applications = NON_V66_AI_APPLICATIONS
        self.applications = NON_AI_APPLICATIONS + architecture_applications
        self.application_select = Gtk.ComboBoxText()
        for app in self.applications:
            self.application_select.append_text(app)

        # Set the default active application
        self.application_select.set_active(len(NON_AI_APPLICATIONS))

        # Application descriptions dictionary
        self.app_descriptions = {
            "Record live video": "Records camera feed and saves video to local storage.",
            "DashCamera": "Simultaneous streaming from two camera sensors.",
            "VideoWall": "Multichannel video decoding and composition. Creates four instances of a video. Requires 'Record live video' to be executed first.",
            "ObjectDetection": "Object detection on input streams from a camera using YOLOX model.",
            "Parallel-AI-Fusion": "Parallel AI inferences (Detection, Classification, Pose estimation and Segmentation) from a camera.",
            "Classification": "Classification on input streams from a camera.",
            "Face Detection": "Face detection on input streams from a camera.",
            "Daisychain Pose": "Daisychain of detection and pose estimation from a camera.",
            "Pose Detection": "Pose detection on input streams from a camera.",
            "Multistream": "Multistream inference from a camera and 3 file streams.",
            "Gesture Recognition": "Palm detection, hand landmark tracking and gesture classification from a camera.",
            "VideoTestSrc": "Displays a test video pattern (no camera required)."
        }

        # Create a description label that updates when the user selects an application
        self.desc_label = Gtk.Label(label="Description:")
        self.desc_value = Gtk.Label()
        self.desc_value.set_line_wrap(True)
        self.desc_value.set_max_width_chars(40)
        self.desc_value.set_xalign(0)
        self.desc_value.set_text(self.app_descriptions.get(self.application_select.get_active_text(), ""))

        # Connect application selection change to update description
        self.application_select.connect("changed", self.on_application_changed)

        # Create a "Start" button
        self.start = Gtk.Button.new_with_label("Start")
        self.start.connect("clicked", self.on_start_button_clicked)

        # Add the label to the grid
        grid.attach(self.label, 0, 0, 1, 1)
        grid.attach(self.status, 1, 0, 1, 1)
        grid.attach(self.ssid_label, 0, 1, 1, 1)
        grid.attach(self.ssid_value, 1, 1, 1, 1)
        grid.attach(self.ip_label, 0, 2, 1, 1)
        grid.attach(self.ip_value, 1, 2, 1, 1)
        grid.attach(self.src_label, 0, 3, 1, 1)
        grid.attach(self.app_label, 0, 4, 1, 1)

        # Add the ComboBoxText to the grid
        grid.attach(self.src_select, 1, 3, 1, 1)
        grid.attach(self.application_select, 1, 4, 1, 1)

        # Attach the description label and value to the grid
        grid.attach(self.desc_label, 0, 5, 1, 1)
        grid.attach(self.desc_value, 1, 5, 1, 1)

        # Attach the "Start" button to the grid
        grid.attach(self.start, 1, 6, 1, 1)

        # Attach the "Exit" button to the grid
        grid.attach(self.exit_app, 1, 7, 1, 1)

        # Add an empty label to create a gap
        grid.attach(Gtk.Label(), 1, 8, 1, 1)

        # Add the grid to the window
        self.add(grid)

    def get_soc_id(self):
        """Read the SoC ID from sysfs. Returns 0 and shows a warning on failure."""
        soc_path = SOC_DEV_PATH_PRIMARY if os.path.exists(SOC_DEV_PATH_PRIMARY) else SOC_DEV_PATH_SECONDARY
        try:
            with open(soc_path, "r", encoding="ascii") as soc_file:
                return int(soc_file.read().strip())
        except (OSError, ValueError) as exc:
            print(f"WARNING: Failed to read SoC ID from {soc_path}: {exc}")
            return 0

    def is_v66_arch(self, soc_id: int):
        """Return True if the given SoC ID belongs to a v66 architecture."""
        return soc_id in V66_SOC_IDS.values()

    def check_camx_availability(self):
        """Check whether the CamX socket exists on this device."""
        return os.access(CAMX_SOCKET_PATH, os.F_OK)

    def update_ip_address(self, device):
        result = subprocess.run(
            ['nmcli', '-t', '-f', 'IP4.ADDRESS', 'device', 'show', device],
            capture_output=True, text=True
        )

        ip_address = None

        for line in result.stdout.splitlines():
            if 'IP4.ADDRESS[1]:' in line:
                ip_address = line.split(':')[1].split('/')[0].strip()
                break

        if ip_address:
            self.ip_value.set_text(ip_address)
        else:
            self.ip_value.set_text("Not connected")

    def establish_connection(self, widget):
        # Create a dialog window for connecting to Wi-Fi
        dialog = Gtk.Dialog(title="Connect to Wi-Fi", transient_for=self, modal=True, destroy_with_parent=True)
        dialog.add_buttons(Gtk.STOCK_OK, Gtk.ResponseType.OK, Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL)

        # Get the content area of the dialog and set spacing between elements
        box = dialog.get_content_area()
        box.set_spacing(10)

        # Create and add a label and entry for the SSID
        ssid_label = Gtk.Label(label="SSID:")
        ssid_entry = Gtk.Entry()
        box.pack_start(ssid_label, True, True, 0)
        box.pack_start(ssid_entry, True, True, 0)

        # Create and add a label and entry for the password
        password_label = Gtk.Label(label="Password:")
        password_entry = Gtk.Entry()
        password_entry.set_visibility(False)
        box.pack_start(password_label, True, True, 0)
        box.pack_start(password_entry, True, True, 0)

        # Show all elements in the dialog
        dialog.show_all()
        response = dialog.run()

        # If the user clicked OK, get the SSID and password, and connect to Wi-Fi
        if response == Gtk.ResponseType.OK:
            self.ssid = ssid_entry.get_text()
            password = password_entry.get_text()
            self.connect_to_wifi(password)

        dialog.destroy()

    def connect_to_wifi(self, password):
        print(f"Connecting to Wi-Fi SSID: {self.ssid}")
        try:
            # Run the nmcli command to connect to the specified Wi-Fi network with the provided password
            subprocess.run(['nmcli', 'dev', 'wifi', 'connect', self.ssid, 'password', password], capture_output=True, text=True, check=True)
            print(f"Connected to Wi-Fi SSID: {self.ssid}")
            self.status.set_text("Wi-Fi is connected")
            # Set text color to green
            self.status.override_color(Gtk.StateFlags.NORMAL, Gdk.RGBA(0, 1, 0, 1))
            self.ssid_value.set_text(self.ssid)
        except subprocess.CalledProcessError as e:
            print(f"Failed to connect to Wi-Fi SSID: {self.ssid} with password: {password}")
            print(f"STDERR: {e.stderr}")
            # Set text color to red
            self.status.override_color(Gtk.StateFlags.NORMAL, Gdk.RGBA(1, 0, 0, 1))

    def disconnect_from_wifi(self, ssid):
        try:
            # Run the nmcli command to disconnect from the specified Wi-Fi network
            subprocess.run(['nmcli', 'connection', 'down', self.ssid_value.get_text()], capture_output=True, text=True, check=True)
            print(f"Disconnected from Wi-Fi SSID: {self.ssid_value.get_text()}")
            self.status.set_text("Wi-Fi is disconnected")
            # Set text color to red
            self.status.override_color(Gtk.StateFlags.NORMAL, Gdk.RGBA(1, 0, 0, 1))
            self.ip_value.set_text("Not connected")
        except subprocess.CalledProcessError as e:
            print(f"Failed to disconnect from Wi-Fi SSID: {self.ssid_value.get_text()}")

    def check_wifi_status(self):
        # Run the nmcli command to get the status of all network devices
        result = subprocess.run(['nmcli', '-t', '-f', 'DEVICE,TYPE,STATE,CONNECTION', 'device'], capture_output=True, text=True)
        wifi_connected = False
        wifi_device = None

        # Parse the command output line by line
        for line in result.stdout.splitlines():
            parts = line.split(':')
            # Check if the line has the expected format and if the device is a connected Wi-Fi
            if len(parts) == 4 and parts[1] == 'wifi' and parts[2] == 'connected':
                wifi_connected = True
                wifi_device = parts[0]
                self.ssid = parts[3]
                break

        if wifi_connected:
            self.status.set_text("Online")
            self.status.override_color(Gtk.StateFlags.NORMAL, Gdk.RGBA(0, 1, 0, 1))
            self.ssid_value.set_text(self.ssid)
            self.update_ip_address(wifi_device)
        else:
            self.status.set_text("Offline")
            # Set text color to red
            self.status.override_color(Gtk.StateFlags.NORMAL, Gdk.RGBA(1, 0, 0, 1))
            self.ssid_value.set_text("Not connected")
            self.ip_value.set_text("Not connected")
            self.ssid = None

        return True

    def show_help(self, widget):
        # Create a message dialog for displaying application information
        dialog = Gtk.MessageDialog(
            transient_for=self,
            flags=0,
            message_type=Gtk.MessageType.INFO,
            buttons=Gtk.ButtonsType.OK,
            text="Applications Information",
        )
        app_information = "\n\n".join(
            f"{application}: {self.app_descriptions[application]}"
            for application in self.applications
        )
        dialog.format_secondary_text(app_information)
        dialog.run()
        dialog.destroy()

    def on_application_changed(self, widget):
        """Update the description label when the user selects a different application."""
        selected_app = widget.get_active_text()
        if selected_app and selected_app in self.app_descriptions:
            self.desc_value.set_text(self.app_descriptions[selected_app])
        else:
            self.desc_value.set_text("")

    def _destroy_popup(self):
        """Safely destroy the download popup if it exists."""
        if getattr(self, "popup", None) is not None:
            self.popup.destroy()
            self.popup = None

    def download_assets(self, application):
        """
        Downloads necessary assets for the specified application if they do not already exist.

        Args:
            application (str): The name of the application to download assets for.
        """

        file_path = os.path.join(QDEMO_HOME, "download_artifacts_2.x.sh")

        # Collect models and labels needed by all AI applications supported on this architecture
        ai_applications = V66_AI_APPLICATIONS if self.v66_arch else NON_V66_AI_APPLICATIONS
        required_models = []
        required_labels = []
        for ai_application in ai_applications:
            requirements = APP_REQUIREMENTS.get(ai_application, {})
            required_models.extend(requirements.get("models", []))
            required_labels.extend(requirements.get("labels", []))
        required_models = list(dict.fromkeys(required_models))
        required_labels = list(dict.fromkeys(required_labels))

        # Build list of required file paths
        required_files = [file_path]
        for model in required_models:
            required_files.append(os.path.join(QDEMO_MODELS_DIR, model))
        for label in required_labels:
            required_files.append(os.path.join(QDEMO_LABELS_DIR, label))
        for media in COMMON_MEDIA:
            required_files.append(os.path.join(QDEMO_MEDIA_DIR, media))

        # Check if all required files exist
        all_files_exist = all(os.path.exists(f) for f in required_files)

        if all_files_exist:
            print(f"All required artifacts for {application} already exist.")
            # Close the popup window
            GLib.idle_add(self._destroy_popup)

            # Set the flag to indicate that artifacts are already downloaded
            self.download_artifacts = True
        else:
            missing_files = [f for f in required_files if not os.path.exists(f)]
            print(f"Missing artifacts: {len(missing_files)} files")
            if self.ssid is None:
                # Close the current popup window
                GLib.idle_add(self._destroy_popup)

                # Create a new popup window for WiFi connection
                self.wifi_popup = Gtk.Window(type=Gtk.WindowType.POPUP)

                # Set the parent window for the new popup
                self.wifi_popup.set_transient_for(self)

                # Set the default size of the new popup window
                self.wifi_popup.set_default_size(200, 150)

                # Position the new popup window at the center of the parent window
                self.wifi_popup.set_position(Gtk.WindowPosition.CENTER_ON_PARENT)

                # Move the new popup window to a specific location
                self.wifi_popup.move(10, 10)

                # Add a label to the new popup window
                label = Gtk.Label(label="Connect to network")
                self.wifi_popup.add(label)

                # Show all widgets in the new popup window
                self.wifi_popup.show_all()

                # Close the new popup window after a specified timeout (3 seconds)
                GLib.timeout_add_seconds(3, self.close_popup)

                # Set the flag to indicate that artifacts are not downloaded
                self.download_artifacts = False
            else:
                try:
                    # Show the popup window
                    self.popup.show_all()

                    # URL of the file to be downloaded
                    url = "https://raw.githubusercontent.com/qualcomm/sample-apps-for-qualcomm-linux/refs/heads/main/qualcomm-linux/scripts/download_artifacts_2.x.sh"

                    # Ensure qdemo directories exist
                    setup_qdemo_dirs()

                    # Download the file from the URL to the specified file path
                    urllib.request.urlretrieve(url, file_path)
                    print("File downloaded successfully!")

                    # Change the file permissions to make it executable
                    os.chmod(file_path, 0o755)

                    # Start a subprocess to execute the downloaded file with specified arguments
                    # Set QDEMO_HOME environment variable so the script knows where to place artifacts
                    env = os.environ.copy()
                    env["QDEMO_HOME"] = QDEMO_HOME

                    self.process = subprocess.Popen(
                        [file_path, "--qdemo"],
                        start_new_session=True,
                        env=env,
                    )

                    # Wait for the subprocess to complete
                    self.process.wait()
                    self.process = None

                    # Set the flag to indicate that artifacts are downloaded
                    self.download_artifacts = True

                except Exception as e:
                    print(f"Error during artifact download: {e}")
                    self.download_artifacts = False
                    self.check_download = None
                    self.process = None
                finally:
                    # Close the popup window
                    GLib.idle_add(self.popup.destroy)

    def close_popup(self):
        self.wifi_popup.destroy()
        # Returning False to stop the timeout
        return False

    def check_usb_driver(self):
        for i in range(MAX_VIDEO_DEVICES):
            device = f'/dev/video{i}'
            try:
                # Run the udevadm command and capture the output
                result = subprocess.run(['udevadm', 'info', '-n', device], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

                # Check if the command was successful
                if result.returncode != 0:
                    print(f"Error running udevadm for {device}: {result.stderr}")
                    continue

                # Check if the ID_USB_DRIVER=uvcvideo field is present in the output
                if 'ID_USB_DRIVER=uvcvideo' in result.stdout:
                    print(f"The device {device} is using the uvcvideo driver.")
                    return device
                else:
                    print(f"The device {device} is not using the uvcvideo driver.")
            except Exception as e:
                print(f"An error occurred while checking {device}: {e}")

        # If no device is found, show a GTK pop-up window
        class MessageWindow(Gtk.Window):
            def __init__(self):
                Gtk.Window.__init__(self, title="USB Camera Not Found")
                self.set_border_width(10)
                self.set_default_size(300, 100)

                label = Gtk.Label(label="No USB camera found. Please attach a USB camera.")

                vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
                vbox.pack_start(label, True, True, 0)

                self.add(vbox)

        win = MessageWindow()
        win.connect("destroy", Gtk.main_quit)
        win.show_all()
        Gtk.main()

        return None

    def create_pipeline(self, application, src):
        if self.check_download is not None:
            self.check_download.join()
        pipeline = None
        if self.v66_arch:
            TFLITE_DELEGATE = (
                "delegate=external "
                "external-delegate-path=libQnnTFLiteDelegate.so "
                "external-delegate-options='QNNExternalDelegate,"
                "backend_type=dsp'"
            )
        else:
            TFLITE_DELEGATE = (
                "delegate=external "
                "external-delegate-path=libQnnTFLiteDelegate.so "
                "external-delegate-options='QNNExternalDelegate,"
                "backend_type=htp,"
                "htp_device_id=(string)0,"
                "htp_performance_mode=(string)2'"
            )

        # Artifact paths
        models_dir = QDEMO_MODELS_DIR
        labels_dir = QDEMO_LABELS_DIR
        media_dir = QDEMO_MEDIA_DIR

        video_out = os.path.join(media_dir, "video_out.mp4")
        video_in = os.path.join(media_dir, "video.mp4")

        if src == "USB-Camera":
            found_device = self.check_usb_driver()
            if found_device == None:
                return None
            in_src = f"v4l2src io-mode=dmabuf-import device={found_device} ! video/x-raw,width=640,height=360,framerate=30/1 ! queue ! qtivtransform ! video/x-raw,format=NV12"
        elif src == "On-Device-Camera":
            if self.check_camx_availability():
                camera_plugin = "qticamsrc name=camsrc"
            else:
                camera_plugin = "libcamerasrc"
            in_src = f"{camera_plugin} ! qtivtransform ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1"
        if application == "Record live video":
            if not os.path.exists(media_dir):
                os.makedirs(media_dir, exist_ok=True)

            pipeline = "gst-launch-1.0 " + in_src + " ! queue ! tee name=split ! queue ! qtivcomposer ! queue ! \
                gtksink split. ! queue ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! \
                h264parse ! mp4mux reserved-moov-update-period=1000000000 reserved-bytes-per-sec=10000 reserved-max-duration=600000000000 ! filesink location=" + video_out

        elif application == "DashCamera" and src == "On-Device-Camera":
            pipeline = "gst-launch-1.0 -e qtivcomposer name=mix sink_0::position='<0, 0>' sink_0::dimensions='<640, 360>' \
                sink_1::position='<640, 0>' sink_1::dimensions='<640, 360>' \
                mix. ! queue ! gtksink \
                qticamsrc name=camsrc camera=0 ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 ! queue ! mix. \
                qticamsrc name=camsrc1 camera=1 ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 ! queue ! mix. "

        elif application == "DashCamera" and src == "USB-Camera":
            pipeline = f"gst-launch-1.0 -e qtivcomposer name=mix sink_0::position='<0, 0>' sink_0::dimensions='<640, 360>' \
                sink_1::position='<640, 0>' sink_1::dimensions='<640, 360>' \
                mix. ! queue ! gtksink " + in_src + "! queue ! tee name=split ! queue ! mix. \
                split. ! queue ! mix. "

        elif application == "VideoWall":
            if not os.path.exists(video_out):
                # Create a new popup window
                popup = Gtk.Window(type=Gtk.WindowType.POPUP)

                # Set the parent window for the new popup
                popup.set_transient_for(self)

                # Set the default size of the new popup window
                popup.set_default_size(400, 50)

                # Position the new popup window at the center of the parent window
                popup.set_position(Gtk.WindowPosition.CENTER_ON_PARENT)

                # Move the new popup window to a specific location
                popup.move(20, 250)

                # Add a label to the new popup window
                label = Gtk.Label(label="Input file is missing. Execute 'Record live video' to save file")
                popup.add(label)

                # Show all widgets in the new popup window
                popup.show_all()

                # Closes after 3 seconds
                GLib.timeout_add_seconds(2, popup.destroy)
            else:
                pipeline = f"gst-launch-1.0 -e qtivcomposer name=mix \
                    sink_0::position='<0, 0>' sink_0::dimensions='<640, 360>' \
                    sink_1::position='<640, 0>' sink_1::dimensions='<640, 360>' \
                    sink_2::position='<0, 360>' sink_2::dimensions='<640, 360>' \
                    sink_3::position='<640, 360>' sink_3::dimensions='<640, 360>' \
                    mix. ! queue ! gtksink \
                    filesrc location={video_out} ! qtdemux ! queue ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! mix. \
                    filesrc location={video_out} ! qtdemux ! queue ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! mix. \
                    filesrc location={video_out} ! qtdemux ! queue ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! mix. \
                    filesrc location={video_out} ! qtdemux ! queue ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! mix."

        elif application == "VideoTestSrc":
            pipeline = "gst-launch-1.0 " + "videotestsrc" + " ! " + "video/x-raw,width=640" + ",height=480" + " ! " + "gtksink"

        if self.download_artifacts is True:
            if application == "Parallel-AI-Fusion" :
                pipeline = f"gst-launch-1.0 \
                    qtivcomposer name=mixer \
                    sink_0::position='<0, 0>' sink_0::dimensions='<960, 540>' \
                    sink_1::position='<0, 0>' sink_1::dimensions='<960, 540>' \
                    sink_2::position='<960, 0>' sink_2::dimensions='<960, 540>' \
                    sink_3::position='<990, 30>' sink_3::dimensions='<360, 48>' \
                    sink_4::position='<0, 540>' sink_4::dimensions='<960, 540>' \
                    sink_5::position='<0, 540>' sink_5::dimensions='<960, 540>' \
                    sink_6::position='<960, 540>' sink_6::dimensions='<960, 540>' \
                    sink_7::position='<960, 540>' sink_7::dimensions='<960, 540>' sink_7::alpha=0.5 \
                    mixer. ! queue ! fpsdisplaysink signal-fps-measurements=true text-overlay=true video-sink='gtksink' " + in_src + f"! queue ! tee name=split \
                    split. ! queue ! mixer. \
                    split. ! queue ! qtimlvconverter ! queue ! qtimltflite {TFLITE_DELEGATE} model={models_dir}/yolox_quantized.tflite ! queue ! qtimlpostprocess settings='{{\"confidence\": 51.0}}' results=10 module=yolov8 labels={labels_dir}/yolox.json ! video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer. \
                    split. ! queue ! mixer. \
                    split. ! queue ! qtimlvconverter ! queue ! qtimltflite {TFLITE_DELEGATE} model={models_dir}/inception_v3_quantized.tflite ! queue ! qtimlpostprocess settings='{{\"confidence\": 40.0}}' results=2 module=mobilenet-softmax labels={labels_dir}/classification.json ! video/x-raw,format=RGBA ! queue ! mixer. \
                    split. ! queue ! mixer. \
                    split. ! queue ! qtimlvconverter ! queue ! qtimltflite {TFLITE_DELEGATE} model={models_dir}/hrnet_pose_quantized.tflite ! queue ! qtimlpostprocess settings={labels_dir}/hrnet_pose_settings.json results=2 module=hrnet labels={labels_dir}/hrnet_pose.json ! video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer. \
                    split. ! queue ! mixer. \
                    split. ! queue ! qtimlvconverter ! queue ! qtimltflite {TFLITE_DELEGATE} model={models_dir}/deeplabv3_plus_mobilenet_quantized.tflite ! queue ! qtimlpostprocess module=deeplab-argmax labels={labels_dir}/deeplabv3_resnet50.json ! video/x-raw,format=RGBA ! queue ! mixer. \
                    "
            elif application == "ObjectDetection":
                pipeline = "gst-launch-1.0 " + in_src + f"! queue ! tee name=split \
                    split. ! queue ! qtivcomposer name=mixer ! queue ! gtksink \
                    split. ! queue ! qtimlvconverter ! queue ! qtimltflite {TFLITE_DELEGATE} model='{models_dir}/yolox_quantized.tflite' ! queue ! \
                    qtimlpostprocess settings='{{\"confidence\": 51.0}}' results=10 module=yolov8 labels='{labels_dir}/yolox.json' ! \
                    video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer."
            elif application == "Classification":
                pipeline = "gst-launch-1.0 " + in_src + f" ! queue ! tee name=split \
                    split. ! queue ! qtivcomposer name=mixer ! queue ! gtksink \
                    split. ! queue ! qtimlvconverter ! queue ! qtimltflite {TFLITE_DELEGATE} model='{models_dir}/inception_v3_quantized.tflite' ! queue ! \
                    qtimlpostprocess settings='{{\"confidence\": 40.0}}' results=2 module=mobilenet-softmax labels='{labels_dir}/classification.json' ! \
                    video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer."
            elif application == "Face Detection":
                if src == "On-Device-Camera":
                    camera_plugin = "qticamsrc name=camsrc" if self.check_camx_availability() else "libcamerasrc"
                    in_src = f"{camera_plugin} ! qtivtransform ! video/x-raw,format=NV12,width=640,height=480,framerate=30/1"
                else:
                    in_src = f"v4l2src io-mode=dmabuf-import device={found_device} ! video/x-raw,width=640,height=480,framerate=30/1 ! queue ! qtivtransform ! video/x-raw,format=NV12"
                if self.v66_arch:
                    in_src += " ! videorate ! video/x-raw,framerate=15/1"
                pipeline = f"gst-launch-1.0 -e \
                    qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
                    qtimltflite name=stage_01_inference model={models_dir}/face_det_lite_quantized.tflite {TFLITE_DELEGATE} \
                    qtimlpostprocess name=stage_01_postproc settings='{{\"confidence\": 51.0}}' results=6 module=qfd labels={labels_dir}/face_detection.json \
                    " + in_src + " ! queue ! tee name=split ! queue ! qtivcomposer name=mixer ! queue ! gtksink \
                    split. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! video/x-raw ! queue ! mixer."
            elif application == "Pose Detection":
                if src == "On-Device-Camera":
                    camera_plugin = "qticamsrc name=camsrc" if self.check_camx_availability() else "libcamerasrc"
                    in_src = f"{camera_plugin} ! video/x-raw,width=640,height=480,framerate=15/1"
                else:
                    in_src = f"v4l2src io-mode=dmabuf-import device={found_device} ! video/x-raw,width=640,height=480,framerate=15/1"
                pipeline = "gst-launch-1.0 -e " + in_src + f" ! qtivtransform ! video/x-raw ! queue ! tee name=split \
                    split. ! queue ! qtivcomposer name=mixer ! queue ! gtksink sync=false \
                    split. ! queue ! qtimlvconverter ! queue ! qtimltflite {TFLITE_DELEGATE} model='{models_dir}/hrnet_pose_quantized.tflite' ! queue ! \
                    qtimlpostprocess results=2 module=hrnet labels='{labels_dir}/hrnet_pose.json' settings='{labels_dir}/hrnet_pose_settings.json' ! \
                    video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer."
            elif application == "Daisychain Pose":
                pipeline = f"gst-launch-1.0 -e \
                    qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
                    qtimltflite name=stage_01_inference {TFLITE_DELEGATE} model='{models_dir}/yolox_quantized.tflite' \
                    qtimlpostprocess name=stage_01_postproc settings='{{\"confidence\": 51.0}}' results=4 module=yolov8 labels={labels_dir}/yolox.json \
                    qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative image-disposition=centre  \
                    qtimltflite name=stage_02_inference {TFLITE_DELEGATE} model={models_dir}/hrnet_pose_quantized.tflite \
                    qtimlpostprocess name=stage_02_postproc settings={labels_dir}/hrnet_pose_settings.json results=1 module=hrnet labels={labels_dir}/hrnet_pose.json \
                    " + in_src + " ! queue ! tee name=t_split_1 \
                    t_split_1. ! queue ! metamux_1. \
                    t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
                    qtimetamux name=metamux_1 ! queue ! tee name=t_split_2 \
                    t_split_2. ! queue ! metamux_2. \
                    t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
                    qtimetamux name=metamux_2 ! queue ! tee name=t_split_3 \
                    t_split_3. ! queue ! mixer. \
                    t_split_3. ! queue ! qtivsplit name=vsplit src_0::mode=single-roi-meta src_1::mode=single-roi-meta \
                    vsplit. ! video/x-raw,width=240,height=480,format=RGBA ! queue ! mixer. \
                    vsplit. ! video/x-raw,width=240,height=480,format=RGBA ! queue ! mixer. \
                    qtivcomposer name=mixer \
                    sink_0::position='<0, 0>' sink_0::dimensions='<1920, 1080>' \
                    sink_1::position='<0, 0>' sink_1::dimensions='<240, 480>' \
                    sink_2::position='<1680, 0>' sink_2::dimensions='<240, 480>' \
                    mixer. ! queue ! qtivoverlay ! gtksink"
            elif application == "Gesture Recognition":
                os.environ["OCL_ICD_FILENAMES"] = "/usr/lib/libOpenCL_adreno.so.1"
                framerate = "10" if self.v66_arch else "30"
                if src == "On-Device-Camera":
                    camera_plugin = "qticamsrc name=camsrc" if self.check_camx_availability() else "libcamerasrc"
                    in_src = f"{camera_plugin} ! video/x-raw,width=640,height=480,framerate={framerate}/1"
                else:
                    in_src = f"v4l2src io-mode=dmabuf-import device={found_device} ! video/x-raw,width=640,height=480,framerate={framerate}/1"
                pipeline = f"gst-launch-1.0 -e \
                    qtimlvconverter name=palm_pre \
                    qtimltflite name=palm_inf delegate=gpu model={models_dir}/palm_detection.tflite \
                    qtimlpostprocess name=palm_post module=palmd labels={labels_dir}/palmd_labels.json settings={labels_dir}/palmd_settings.json results=1 \
                    qtimetatransform name=metatransform module=roi-palmd \
                    qtimlvconverter name=hand_pre mode=roi-batch-non-cumulative \
                    qtimltflite name=hand_inf delegate=gpu model={models_dir}/hand_landmark.tflite \
                    qtimlpostprocess name=hand_post module=hlandmark labels={labels_dir}/hlandmark_labels.json settings={labels_dir}/hlandmark_settings.json results=6 \
                    qtimlpostprocess name=gesture_pre module=tensor results=6 \
                    qtimltflite name=gesture_embed delegate=gpu model={models_dir}/gesture_embedder.tflite \
                    qtimltflite name=gesture_class delegate=gpu model={models_dir}/canned_gesture_classifier.tflite \
                    qtimlpostprocess name=gesture_post module=mobilenet labels={labels_dir}/gesture_labels.json results=8 \
                    " + in_src +  " ! qtivtransform ! video/x-raw ! queue ! tee name=t_split_1 \
                    t_split_1. ! queue ! metamux_1. \
                    t_split_1. ! queue ! palm_pre. palm_pre. ! queue ! palm_inf. palm_inf. ! queue ! palm_post. palm_post. ! text/x-raw ! queue ! metamux_1. \
                    qtimetamux name=metamux_1 ! queue ! metatransform. metatransform. ! queue ! tee name=t_split_2 \
                    t_split_2. ! queue ! metamux_2. \
                    t_split_2. ! queue ! hand_pre. hand_pre. ! queue ! hand_inf. hand_inf. ! queue ! tee name=t_split_3 \
                    t_split_3. ! queue ! hand_post. hand_post. ! text/x-raw ! queue ! metamux_2. \
                    t_split_3. ! queue ! gesture_pre. gesture_pre. ! queue ! gesture_embed. gesture_embed. ! queue ! gesture_class. gesture_class. ! queue ! gesture_post. gesture_post. ! text/x-raw ! queue ! metamux_2. \
                    qtimetamux name=metamux_2 ! queue ! qtivoverlay ! queue ! gtksink sync=false"
            elif application == "Multistream":
                pipeline = f"gst-launch-1.0 -e qtivcomposer name=mixer \
                    sink_0::position='<0, 0>' sink_0::dimensions='<960, 540>' \
                    sink_1::position='<960, 0>' sink_1::dimensions='<960, 540>' \
                    sink_2::position='<0, 540>' sink_2::dimensions='<960, 540>' \
                    sink_3::position='<960, 540>' sink_3::dimensions='<960, 540>' \
                    sink_4::position='<0, 0>' sink_4::dimensions='<960, 540>' \
                    sink_5::position='<960, 0>' sink_5::dimensions='<960, 540>' \
                    sink_6::position='<0, 540>' sink_6::dimensions='<960, 540>' \
                    sink_7::position='<960, 540>' sink_7::dimensions='<960, 540>' \
                    mixer. ! queue ! gtksink \
                    split_1. ! qtimlvconverter ! qtimltflite name=stage_01_inference {TFLITE_DELEGATE} model={models_dir}/yolox_quantized.tflite \
                    split_2. ! qtimlvconverter ! qtimltflite name=stage_02_inference {TFLITE_DELEGATE} model={models_dir}/yolox_quantized.tflite \
                    split_3. ! qtimlvconverter ! qtimltflite name=stage_03_inference {TFLITE_DELEGATE} model={models_dir}/yolox_quantized.tflite \
                    split_4. ! qtimlvconverter ! qtimltflite name=stage_04_inference {TFLITE_DELEGATE} model={models_dir}/yolox_quantized.tflite \
                    " + in_src + f" ! tee name=split_1 ! queue ! mixer. \
                    filesrc location={video_in} ! qtdemux ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! tee name=split_2 ! queue ! mixer. \
                    filesrc location={video_in} ! qtdemux ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! tee name=split_3 ! queue ! mixer. \
                    filesrc location={video_in} ! qtdemux ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! tee name=split_4 ! queue ! mixer. \
                    stage_01_inference. ! queue ! qtimlpostprocess settings='{{\"confidence\": 51.0}}' results=10 module=yolov8 labels={labels_dir}/yolox.json ! video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer. \
                    stage_02_inference. ! queue ! qtimlpostprocess settings='{{\"confidence\": 51.0}}' results=10 module=yolov8 labels={labels_dir}/yolox.json ! video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer. \
                    stage_03_inference. ! queue ! qtimlpostprocess settings='{{\"confidence\": 51.0}}' results=10 module=yolov8 labels={labels_dir}/yolox.json ! video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer. \
                    stage_04_inference. ! queue ! qtimlpostprocess settings='{{\"confidence\": 51.0}}' results=10 module=yolov8 labels={labels_dir}/yolox.json ! video/x-raw,format=RGBA,width=640,height=360 ! queue ! mixer. "
        if pipeline is not None:
            pipeline_thread = threading.Thread(target=self.execute, args=(pipeline,))
            pipeline_thread.start()

    def on_start_button_clicked(self, widget):
        """
        Handles the start action, creating a popup window with an animated GIF,
        downloading necessary assets, and starting the application pipeline.
        """
        # Create the popup window
        self.popup = Gtk.Window(type=Gtk.WindowType.POPUP)

        # Set to the parent window
        self.popup.set_transient_for(self)
        self.popup.set_default_size(450, 300)
        self.popup.move(55, 0)

        # Load the animated GIF
        gif_path = "/usr/share/qdemo/Qdemo.gif"
        animation = GdkPixbuf.PixbufAnimation.new_from_file(gif_path)
        gif_image = Gtk.Image.new_from_animation(animation)

        # Create a box to hold the GIF and the close button
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        vbox.set_name("vbox")

        # Create and add the header
        headerlabel = Gtk.Label(label="Downloading models and labels")
        headerlabel.set_name("headerlabel")
        vbox.pack_start(headerlabel, False, False, 0)
        vbox.pack_start(gif_image, True, True, 0)

        # Create the cancel button
        cancel_button = Gtk.Button(label="cancel")
        cancel_button.connect("clicked", self.on_cancel_button_clicked)
        vbox.pack_start(cancel_button, False, False, 0)
        self.popup.add(vbox)

        # Apply CSS styling
        css_provider = Gtk.CssProvider()
        css_provider.load_from_data(b"""
            #vbox {
                border: 1px solid black;
            }
            #headerlabel {
                padding: 10px;
                background-color: #2196F3;
                color: blue;
            }
        """)
        style_context = Gtk.StyleContext()
        screen = Gdk.Screen.get_default()
        style_context.add_provider_for_screen(screen, css_provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        # Add the box to the popup window
        self.popup.show_all()
        self.popup.present_with_time(Gdk.CURRENT_TIME)

        # Get the selected application
        application = self.application_select.get_active_text()
        src = self.src_select.get_active_text()

        # Download models and labels
        if application in AI_APPLICATIONS:
            self.check_download = threading.Thread(target=self.download_assets, args=(application,))
            self.check_download.start()
        else:
            self._destroy_popup()

        # Start the application pipeline in a separate thread
        threading.Thread(target=self.create_pipeline, args=(application, src,)).start()

    def on_cancel_button_clicked(self, widget=None):
        # Close the popup window
        self._destroy_popup()

        # Set the flag to indicate that artifacts are not downloaded
        self.download_artifacts = False

        # Reset the check_download attribute
        self.check_download = None

        if self.process is not None:
            try:
                # Terminate the process group associated with the process
                os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)

                # Wait for the process to terminate
                self.process.wait()
            except (ProcessLookupError, OSError):
                pass
            finally:
                self.process = None

    def execute(self, pipeline):
        """
        Executes a given shell command pipeline.

        Args:
            pipeline (str): The shell command pipeline to execute.
        """
        # Run the shell command pipeline
        try:
            subprocess.run(pipeline, shell=True)
        except Exception as e:
            print("Exception while execution")

    def on_exit_button_clicked(self, widget):
        """Exit the application"""
        os.killpg(os.getpgid(os.getpid()), signal.SIGTERM)
        Gtk.main_quit()

if __name__ == "__main__":
    signal.signal(signal.SIGINT, lambda s, f: Gtk.main_quit())
    GLib.timeout_add(500, lambda: True)

    # Create an instance of the DemoWindow class
    demo = DemoWindow()
    demo.connect("destroy", Gtk.main_quit)

    # Show all widgets in the demo window
    demo.show_all()

    # Start the GTK main loop
    Gtk.main()
