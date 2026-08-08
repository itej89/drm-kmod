/*
 * DRM modeset test for DC8200 HDMI output.
 * Opens /dev/dri/card1, finds the HDMI connector, creates a dumb FB,
 * fills it with a color, and sets the mode to trigger the full pipeline.
 *
 * Build: cc -o drm_modeset_test drm_modeset_test.c
 * Run:   ./drm_modeset_test [/dev/dri/card1]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static int drm_ioctl(int fd, unsigned long req, void *arg)
{
	int ret;
	do {
		ret = ioctl(fd, req, arg);
	} while (ret == -1 && errno == EINTR);
	return ret;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/card1";
	int fd, ret;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* Get resources */
	struct drm_mode_card_res res = {0};
	ret = drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	if (ret) {
		perror("GETRESOURCES");
		return 1;
	}

	printf("DRM: %d connectors, %d CRTCs, %d encoders\n",
	       res.count_connectors, res.count_crtcs, res.count_encoders);

	if (res.count_connectors == 0 || res.count_crtcs == 0) {
		printf("No connectors or CRTCs found\n");
		return 1;
	}

	uint32_t conn_ids[8], crtc_ids[8], enc_ids[8];
	res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
	res.encoder_id_ptr = (uint64_t)(uintptr_t)enc_ids;
	ret = drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	if (ret) {
		perror("GETRESOURCES2");
		return 1;
	}

	/* Get connector info */
	struct drm_mode_get_connector conn = {0};
	conn.connector_id = conn_ids[0];
	ret = drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	if (ret) {
		perror("GETCONNECTOR");
		return 1;
	}

	printf("Connector %u: type=%u, status=%u, modes=%u, encoders=%u\n",
	       conn.connector_id, conn.connector_type,
	       conn.connection, conn.count_modes, conn.count_encoders);

	if (conn.count_modes == 0) {
		printf("No modes available\n");
		return 1;
	}

	struct drm_mode_modeinfo modes[8];
	uint32_t enc_id_list[8];
	conn.modes_ptr = (uint64_t)(uintptr_t)modes;
	conn.encoders_ptr = (uint64_t)(uintptr_t)enc_id_list;
	ret = drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	if (ret) {
		perror("GETCONNECTOR2");
		return 1;
	}

	struct drm_mode_modeinfo *mode = &modes[0];
	printf("Mode: %s %ux%u@%uHz\n", mode->name,
	       mode->hdisplay, mode->vdisplay, mode->vrefresh);

	/* Create dumb buffer */
	struct drm_mode_create_dumb create = {0};
	create.width = mode->hdisplay;
	create.height = mode->vdisplay;
	create.bpp = 32;
	ret = drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret) {
		perror("CREATE_DUMB");
		return 1;
	}

	printf("Dumb buffer: %ux%u, pitch=%u, size=%llu, handle=%u\n",
	       create.width, create.height, create.pitch,
	       (unsigned long long)create.size, create.handle);

	/* Create framebuffer */
	struct drm_mode_fb_cmd2 fb_cmd = {0};
	fb_cmd.width = create.width;
	fb_cmd.height = create.height;
	fb_cmd.pixel_format = 0x34325258; /* DRM_FORMAT_XRGB8888 */
	fb_cmd.handles[0] = create.handle;
	fb_cmd.pitches[0] = create.pitch;
	fb_cmd.offsets[0] = 0;
	ret = drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb_cmd);
	if (ret) {
		perror("ADDFB2");
		return 1;
	}

	printf("Framebuffer ID: %u\n", fb_cmd.fb_id);

	/* Map and fill with blue */
	struct drm_mode_map_dumb map = {0};
	map.handle = create.handle;
	ret = drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (ret) {
		perror("MAP_DUMB");
		return 1;
	}

	uint32_t *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, map.offset);
	if (pixels == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	/* Fill: top half blue, bottom half red */
	uint32_t npixels = create.size / 4;
	uint32_t half = npixels / 2;
	for (uint32_t i = 0; i < half; i++)
		pixels[i] = 0x000000FF; /* blue */
	for (uint32_t i = half; i < npixels; i++)
		pixels[i] = 0x00FF0000; /* red */

	printf("Framebuffer filled (blue/red)\n");

	/* Set CRTC */
	struct drm_mode_crtc crtc = {0};
	crtc.crtc_id = crtc_ids[0];
	crtc.fb_id = fb_cmd.fb_id;
	crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_ids[0];
	crtc.count_connectors = 1;
	crtc.mode = *mode;
	crtc.mode_valid = 1;

	printf("Setting CRTC %u with FB %u on connector %u...\n",
	       crtc.crtc_id, fb_cmd.fb_id, conn_ids[0]);

	ret = drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
	if (ret) {
		perror("SETCRTC");
		return 1;
	}

	printf("SUCCESS: Mode set! Check HDMI monitor.\n");
	printf("Press Enter to exit and release display...\n");
	getchar();

	/* Cleanup */
	munmap(pixels, create.size);

	struct drm_mode_destroy_dumb destroy = {0};
	destroy.handle = create.handle;
	drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);

	close(fd);
	return 0;
}
