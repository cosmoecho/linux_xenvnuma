#include <linux/err.h>
#include <linux/memblock.h>
#include <xen/interface/xen.h>
#include <xen/interface/memory.h>
#include <asm/xen/interface.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/vnuma.h>

/*
 * Called from numa_init if numa_off = 0;
 * we set numa_off = 0 if xen_vnuma_supported()
 * returns true and its a domU;
 */
int __init xen_numa_init(void)
{
	int rc;
	unsigned int i, j, cpu, idx, pcpus, nr_nodes;
	u64 physm, physd, physc;
	unsigned int *vdistance, *cpu_to_node, prep_nr_nodes, prep_nr_cpus;
	unsigned long mem_size, dist_size, cpu_to_node_size;
	struct vmemrange *vblock;

	struct vnuma_topology_info numa_topo = {
		.domid = DOMID_SELF
	};

	rc = -EINVAL;
	physm = physd = physc = 0;

	/* For now only PV guests are supported */
	if (!xen_pv_domain())
		return rc;

	/* get the number of nodes for allocation of memblocks */
	pcpus = num_possible_cpus();
	set_xen_guest_handle(numa_topo.nr_nodes.h, &prep_nr_nodes);
	set_xen_guest_handle(numa_topo.nr_cpus.h, &prep_nr_cpus);

	if (HYPERVISOR_memory_op(XENMEM_get_vnodes_vcpus, &numa_topo) < 0)
		goto out;
	/*
	 * In case NR_CPUS < hypervisor vcpus, number of possible
	 * cpus will be set to NR_CPUS and there is no way to learn
	 * number of vcpus that will be copied during next hypercall.
	 * Thus we need to retreive nr_nodes and nr_cpus numbers to
	 * allocate arrays of correct sizes. This also takes into
	 * account kernel boot parameter maxcpus, that can be also smaller
	 * than NR_CPUS and hypervisor vcpus number.
	 * If maxcpus < NR_CPUS and maxcpus < num_possible_cpus, only
	 * maxcpus can be brought up.
	 * In all cases where the allocated arrays are of incorrect size,
	 * the error path is to set dummy numa node.
	 */
	if (prep_nr_nodes == 0 ||
		prep_nr_nodes > setup_max_cpus ||
		prep_nr_nodes > pcpus ||
		pcpus > prep_nr_cpus ||
		prep_nr_cpus == 0)
		goto out;

	mem_size =  prep_nr_nodes * sizeof(struct vmemrange);
	dist_size = prep_nr_nodes * sizeof(*numa_topo.distance.h);
	/* Allocating for size of possible cpus */
	cpu_to_node_size = prep_nr_cpus * sizeof(*numa_topo.cpu_to_node.h);

	physm = memblock_alloc(mem_size, PAGE_SIZE);
	physd = memblock_alloc(dist_size, PAGE_SIZE);
	physc = memblock_alloc(cpu_to_node_size, PAGE_SIZE);

	if (!physm || !physd || !physc)
		goto out;

	vblock = __va(physm);
	vdistance  = __va(physd);
	cpu_to_node  = __va(physc);

	set_xen_guest_handle(numa_topo.nr_nodes.h, &nr_nodes);
	set_xen_guest_handle(numa_topo.memrange.h, vblock);
	set_xen_guest_handle(numa_topo.distance.h, vdistance);
	set_xen_guest_handle(numa_topo.cpu_to_node.h, cpu_to_node);

	if (HYPERVISOR_memory_op(XENMEM_get_vnuma_info, &numa_topo) < 0)
		goto out;

	if (prep_nr_nodes != nr_nodes || prep_nr_cpus != pcpus)
		goto out;
	/*
	 * NUMA nodes memory ranges are in pfns, constructed and
	 * aligned based on e820 ram domain map.
	 */
	for (i = 0; i < nr_nodes; i++) {
		if (numa_add_memblk(i, vblock[i].start, vblock[i].end))
			goto out;
		node_set(i, numa_nodes_parsed);
	}

	setup_nr_node_ids();
	/* Setting the cpu, apicid to node */
	for_each_cpu(cpu, cpu_possible_mask) {
		set_apicid_to_node(cpu, cpu_to_node[cpu]);
		numa_set_node(cpu, cpu_to_node[cpu]);
		cpumask_set_cpu(cpu, node_to_cpumask_map[cpu_to_node[cpu]]);
	}

	for (i = 0; i < nr_nodes; i++) {
		for (j = 0; j < nr_nodes; j++) {
			idx = (j * nr_nodes) + i;
			numa_set_distance(i, j, *(vdistance + idx));
		}
	}

	rc = 0;
out:
	if (physm)
		memblock_free(__pa(physm), mem_size);
	if (physd)
		memblock_free(__pa(physd), dist_size);
	if (physc)
		memblock_free(__pa(physc), cpu_to_node_size);
	/*
	 * Set a dummy node and return success.  This prevents calling any
	 * hardware-specific initializers which do not work in a PV guest.
	 * Taken from dummy_numa_init code.
	 */
	if (rc != 0) {
		for (i = 0; i < MAX_LOCAL_APIC; i++)
			set_apicid_to_node(i, NUMA_NO_NODE);
		nodes_clear(numa_nodes_parsed);
		nodes_clear(node_possible_map);
		nodes_clear(node_online_map);
		node_set(0, numa_nodes_parsed);
		/* cpus up to max_cpus will be assigned to one node */
		numa_add_memblk(0, 0, PFN_PHYS(max_pfn));
		setup_nr_node_ids();
	}
	return 0;
}
