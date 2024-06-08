#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/fdt.h>
#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/fdt.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsdisplayvar.h>
#include <dev/rasops/rasops.h>

#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>

struct asahidrm_softc {
	struct platform_device	sc_dev;
	struct drm_device	sc_ddev;

	int			sc_node;

	struct rasops_info	sc_ri;
	struct wsscreen_descr	sc_wsd;
	struct wsscreen_list	sc_wsl;
	struct wsscreen_descr	*sc_scrlist[1];

	bus_space_tag_t	sc_iot;
	bus_dma_tag_t	sc_dmat;

	void			(*sc_switchcb)(void *, int, int);
	void			*sc_switchcbarg;
	void			*sc_switchcookie;
	struct task		sc_switchtask;

	int			sc_burner_fblank;
	struct task		sc_burner_task;
};


