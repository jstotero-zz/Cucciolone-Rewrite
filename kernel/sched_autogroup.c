struct autogroup {
	struct kref		kref;
	struct task_group	*tg;
	unsigned long		id;
};

unsigned int __read_mostly sysctl_sched_autogroup_enabled = 1;

static struct autogroup autogroup_default;
static atomic_t autogroup_seq_nr;

static void __init autogroup_init(struct task_struct *init_task)
{
	autogroup_default.tg = &init_task_group;
	root_task_group.autogroup = &autogroup_default;
	kref_init(&autogroup_default.kref);
	init_task->signal->autogroup = &autogroup_default;
}

static inline void autogroup_free(struct task_group *tg)
{
	kfree(tg->autogroup);
}

static inline void autogroup_destroy(struct kref *kref)
{
	struct autogroup *ag = container_of(kref, struct autogroup, kref);

	sched_destroy_group(ag->tg);
}

static inline void autogroup_kref_put(struct autogroup *ag)
{
	kref_put(&ag->kref, autogroup_destroy);
}

static inline struct autogroup *autogroup_kref_get(struct autogroup *ag)
{
	kref_get(&ag->kref);
	return ag;
}

static inline struct autogroup *autogroup_create(void)
{
	struct autogroup *ag = kzalloc(sizeof(*ag), GFP_KERNEL);
	struct task_group *tg;

	if (!ag)
		goto out_fail;

	tg = sched_create_group(&root_task_group);

	if (IS_ERR(tg))
		goto out_free;

	kref_init(&ag->kref);
	ag->id = atomic_inc_return(&autogroup_seq_nr);
	ag->tg = tg;
	tg->autogroup = ag;

	return ag;

out_free:
	kfree(ag);
out_fail:
	if (printk_ratelimit()) {
		printk(KERN_WARNING "autogroup_create: %s failure.\n",
			ag ? "sched_create_group()" : "kmalloc()");
	}

	return autogroup_kref_get(&autogroup_default);
}

static inline struct task_group *
autogroup_task_group(struct task_struct *p, struct task_group *tg)
{
	int enabled = ACCESS_ONCE(sysctl_sched_autogroup_enabled);

	enabled &= (tg == &root_task_group);
	enabled &= (p->sched_class == &fair_sched_class);
	enabled &= (!(p->flags & PF_EXITING));

	if (enabled)
		return p->signal->autogroup->tg;

	return tg;
}

static void
autogroup_move_group(struct task_struct *p, struct autogroup *ag)
{
	struct autogroup *prev;
	struct task_struct *t;
	unsigned long flags;

	if (unlikely(!lock_task_sighand(p, &flags))) {
		WARN_ON(1);
		return;
	}

	prev = p->signal->autogroup;
	if (prev == ag) {
		unlock_task_sighand(p, &flags);
		return;
	}

	p->signal->autogroup = autogroup_kref_get(ag);

	t = p;
	do {
		sched_move_task(t);
	} while_each_thread(p, t);

	unlock_task_sighand(p, &flags);
	autogroup_kref_put(prev);
}

/* Must be called with siglock held */
void sched_autogroup_create_attach(struct task_struct *p)
{
	struct autogroup *ag = autogroup_create();

	autogroup_move_group(p, ag);
	/* drop extra refrence added by autogroup_create() */
	autogroup_kref_put(ag);
}
EXPORT_SYMBOL(sched_autogroup_create_attach);

/* Must be called with siglock held.  Currently has no users */
void sched_autogroup_detach(struct task_struct *p)
{
	autogroup_move_group(p, &autogroup_default);
}
EXPORT_SYMBOL(sched_autogroup_detach);

static struct autogroup *autogroup_task_get(struct task_struct *p)
{
	struct autogroup *ag;
	unsigned long flags;

	if (!lock_task_sighand(p, &flags))
		return autogroup_kref_get(&autogroup_default);

	ag = autogroup_kref_get(p->signal->autogroup);
	unlock_task_sighand(p, &flags);

	return ag;
}

void sched_autogroup_fork(struct signal_struct *sig)
{
	sig->autogroup = autogroup_task_get(current);
}

void sched_autogroup_exit(struct signal_struct *sig)
{
	autogroup_kref_put(sig->autogroup);
}

void proc_sched_autogroup_show_task(struct task_struct *p, struct seq_file *m)
{
	struct autogroup *ag = autogroup_task_get(p);

	seq_printf(m, "/autogroup-%ld\n", ag->id);
	autogroup_kref_put(ag);
}
