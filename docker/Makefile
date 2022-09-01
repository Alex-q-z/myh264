TAG=ffmpeg:env

# Build
build:
	docker build --tag=$(TAG) .

build-no_cache:
	docker build --no-cache --tag=$(TAG) .

run:
	docker run -it --rm \
			--ipc=host \
			-v $$(pwd)/..:/home/myh264 \
			$(TAG)
