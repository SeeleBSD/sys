// SPDX-License-Identifier: ISC

#include <drm/bronya/bronyadrm.h>

int	bronyadrm_match(struct device *, void *, void *);
void	bronyadrm_attach(struct device *, struct device *, void *);
int	bronyadrm_activate(struct device *, int);

const struct cfattach bronyadrm_ca = {
	sizeof (struct bronyadrm_softc), bronyadrm_match, bronyadrm_attach,
	NULL, bronyadrm_activate
};

struct cfdriver bronyadrm_cd = {
	NULL, "bronyadrm", DV_DULL
};

void	bronyadrm_attachhook(struct device *);