// SPDX-License-Identifier: ISC

#include <drm/asahi/asahidrm.h>

int	asahidrm_match(struct device *, void *, void *);
void	asahidrm_attach(struct device *, struct device *, void *);
int	asahidrm_activate(struct device *, int);

const struct cfattach asahidrm_ca = {
	sizeof (struct asahidrm_softc), asahidrm_match, asahidrm_attach,
	NULL, asahidrm_activate
};

struct cfdriver asahidrm_cd = {
	NULL, "asahidrm", DV_DULL
};

void	asahidrm_attachhook(struct device *);
