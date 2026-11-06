
.PHONY: build
build: 
	mkdir -p dist
	cd ERCameraZoom \
		&& clang++ -target x86_64-w64-mingw32 -fms-extensions -static-libgcc -static-libstdc++ -shared -o ERCameraZoom.dll *.cpp -lpsapi
	cp ERCameraZoom/ERCameraZoom.dll dist