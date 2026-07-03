BUILD_DIR = build

.PHONY: all build install clean debug restart

all: build

build:
	mkdir -p $(BUILD_DIR)
	# Force /usr prefix so KDE actually finds the plugin
	cd $(BUILD_DIR) && cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DKDE_INSTALL_USE_QT_SYS_PATHS=ON
	$(MAKE) -C $(BUILD_DIR) -j$$(nproc)

install:
	sudo $(MAKE) -C $(BUILD_DIR) install
	# Rebuild KDE system cache
	kbuildsycoca6

clean:
	rm -rf $(BUILD_DIR)

restart:
	systemctl restart --user plasma-krunner.service

# Starts KRunner with extreme plugin verbosity to see exactly why a plugin is rejected
debug:
	systemctl stop --user plasma-krunner.service
	QT_DEBUG_PLUGINS=1 /usr/bin/krunner 2>&1 | grep -i fastapprunner
