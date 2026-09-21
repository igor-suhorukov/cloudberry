//---------------------------------------------------------------------------
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
//	@filename:
//		orca_probe.cpp
//
//	@doc:
//		Reaching the gpdb:: wrapper layer from C, so that SQL can test it.
//
//		The wrappers are C++ and the module is C, so without this nothing
//		could call one until the translator exists -- and the translator is
//		33k lines away.  That would leave the whole layer untested at the
//		moment it is most likely to be wrong: it has just been ported.
//
//		Each probe is the same three things: run inside gpos_exec, because
//		ORCA's pools are keyed to a CTask; catch, because gpos_exec rethrows
//		and a C++ exception reaching a C frame is std::terminate; and copy
//		the answer into the caller's context, because the pool goes when the
//		task ends.  See the note over GpOrcaTraceFlags in orca_api.cpp.
//
//		AND A FOURTH, WHICH BELONGS TO THE CALLER.  When *raised is set
//		because a PostgreSQL error was swallowed -- GP_WRAP catches it with
//		sigsetjmp and re-raises it as a GPOS exception -- PostgreSQL's own
//		error handling never ran.  The transaction is not aborted, the
//		resource owner still holds what the failed call took, and the error
//		stack still has an entry on it.  A caller that then returns a value
//		leaves the backend in that state, and the next thing it does takes
//		the server down.  So every caller of these raises a PostgreSQL error
//		when *raised is set, which is what hands the cleanup back to
//		PostgreSQL.  Cloudberry does the same, in CGPOptimizer, through
//		errstart(ERROR).
//
//		GpOrcaUnportedRaise is the one exception, and it is safe for a
//		reason worth naming: the exception it catches is a GPOS_RAISE from
//		GP_UNPORTED, thrown by C++ that never entered PostgreSQL, so there is
//		no half-finished PostgreSQL error behind it.
//
//		These probe what the *port* changed, not what Cloudberry wrote:
//		the operator OIDs the port had to name itself, the extended-statistics
//		call whose "not built" case PostgreSQL raises on and Cloudberry does
//		not, the two renamed access-method functions, the syscache callback
//		whose signature changed, the one wrapper that gained an answer with
//		the "gp" label, and the raise that stands in for a milestone that has
//		not arrived.
//
//---------------------------------------------------------------------------

extern "C"
{
#include "postgres.h"

#include "nodes/pathnodes.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_policy.h"
}

#include <cstdlib>	// wcstombs

#include "gpos/_api.h"
#include "gpos/error/CException.h"
#include "gpos/memory/CAutoMemoryPool.h"
#include "gpos/string/CWStringDynamic.h"
#include "gpopt/base/CAutoOptCtxt.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "gpopt/mdcache/CMDCache.h"
#include "gpopt/optimizer/COptimizerConfig.h"
#include "naucrates/dxl/CDXLUtils.h"
#include "naucrates/exception.h"
#include "naucrates/md/CMDIdColStats.h"
#include "naucrates/md/CMDIdGPDB.h"
#include "naucrates/md/CMDIdRelStats.h"
#include "naucrates/md/IMDAggregate.h"
#include "naucrates/md/IMDCheckConstraint.h"
#include "naucrates/md/IMDColStats.h"
#include "naucrates/md/IMDFunction.h"
#include "naucrates/md/IMDIndex.h"
#include "naucrates/md/IMDRelStats.h"
#include "naucrates/md/IMDRelation.h"
#include "naucrates/md/IMDScalarOp.h"
#include "naucrates/md/IMDType.h"

#include "CMDProviderRelcache.h"
#include "gp_orca_guc.h"
#include "gpdbwrappers.h"

#include "gp_orca_api.h"

// GPOS_TRY names CErrorHandler without qualification, so it only compiles
// with gpos in scope.  Cloudberry's files do the same thing.
using namespace gpos;

namespace
{
//---------------------------------------------------------------------------
//	What every probe returns: an answer, or the exception raised instead.
//
//	A probe cannot report a GPOS exception by raising one of its own -- it is
//	called from C -- so the two travel together and the caller decides.
//---------------------------------------------------------------------------
struct ProbeResult
{
	Oid			arg = InvalidOid;
	const char *name = nullptr;
	MemoryContext caller = nullptr;

	bool		raised = false;
	gpos::ULONG major = 0;
	gpos::ULONG minor = 0;

	bool		flag = false;
	int			number = 0;
	char	   *text = nullptr;

	/* the aggregate replay at the foot of this file */
	List	   *aggrefs = nullptr;
	int		   *aggnos = nullptr;
	int		   *transnos = nullptr;

	/* the metadata probe: which kind of object, and a column for statistics */
	const char *kind = nullptr;
	int			attno = 0;

	/*
	 * What ORCA said when it raised, for a probe that asks for it.  gpos_exec
	 * sends the task's log to a buffer the caller passes, and an exception is
	 * logged there on its way out -- which is how Cloudberry's COptTasks gets
	 * a fallback's reason, and the only way to get it at all once the task has
	 * unwound.
	 */
	bool		want_message = false;
	char	   *message = nullptr;

	/*
	 * Was the exception a PostgreSQL error that GP_WRAP turned into a GPOS
	 * one?  Then the original is still on PostgreSQL's error stack, and the
	 * caller can re-throw it instead of reporting a generic failure -- which
	 * is what Cloudberry's CGPOptimizer does, and what keeps "cache lookup
	 * failed for type 999999" from arriving as "PG exception raised".
	 */
	bool		from_postgres = false;
};

//	Copy into the caller's context; the pool dies with the task.
char *
CopyOut(ProbeResult *r, const char *s)
{
	if (s == nullptr)
	{
		return nullptr;
	}
	return MemoryContextStrdup(r->caller, s);
}

//	The same for ORCA's wide strings.  NULL if the text will not convert in
//	this backend's LC_CTYPE, which the DXL serializer converted it from.
char *
CopyOutWide(ProbeResult *r, const wchar_t *w)
{
	if (w == nullptr)
	{
		return nullptr;
	}
	size_t		n = wcstombs(nullptr, w, 0);

	if (n == (size_t) -1)
	{
		return nullptr;
	}
	char	   *out = (char *) MemoryContextAlloc(r->caller, n + 1);

	wcstombs(out, w, n + 1);
	return out;
}

//	What gpos_exec logged is a line of ORCA's own format --
//
//	  2026-09-21 03:28:36:955904 UTC,THD000,NOTICE,"<the message>",
//
//	-- sometimes with a stack trace after it.  The quoted part is the only part
//	a reader wants, so keep that and drop the rest, in place.  A buffer in any
//	other shape is left alone rather than guessed at.
void
TrimLogLine(char *line)
{
	if (line == nullptr)
	{
		return;
	}
	char	   *open = strchr(line, '"');

	if (open == nullptr)
	{
		return;
	}
	char	   *close = strstr(open + 1, "\",");

	if (close == nullptr)
	{
		close = strrchr(open + 1, '"');
	}
	if (close == nullptr)
	{
		return;
	}
	size_t		n = (size_t) (close - (open + 1));

	memmove(line, open + 1, n);
	line[n] = '\0';
}

//	Big enough for an exception's message and the log lines before it.
const int	probe_error_buffer_size = 64 * 1024;

//---------------------------------------------------------------------------
//	Run one probe body as a GPOS task, and turn any exception into a flag.
//---------------------------------------------------------------------------
int
RunProbe(void *(*body)(void *), ProbeResult *r)
{
	gpos_exec_params params;
	bool		abort_flag = false;
	int			rc = 0;

	GpOrcaEnsureInitialized();

	memset(&params, 0, sizeof(params));
	params.func = body;
	params.arg = r;
	params.stack_start = &params;
	params.abort_requested = &abort_flag;

	char	   *error_buffer = nullptr;

	if (r->want_message)
	{
		error_buffer = (char *) palloc0(probe_error_buffer_size);
		params.error_buffer = error_buffer;
		params.error_buffer_size = probe_error_buffer_size;
	}

	GPOS_TRY
	{
		rc = gpos_exec(&params);
	}
	GPOS_CATCH_EX(ex)
	{
		r->raised = true;
		r->major = ex.Major();
		r->minor = ex.Minor();
		r->from_postgres = (ex.Major() == gpdxl::ExmaGPDB &&
							ex.Minor() == gpdxl::ExmiGPDBError);
		/*
		 * No GPOS_RESET_EX here, and this is the trap.  It expands to
		 * ITask::Self()->GetErrCtxt()->Reset(), and ITask::Self() is null
		 * outside a task -- which is exactly where this catch runs, because
		 * gpos_exec has already unwound the task by the time it rethrows.
		 * Calling it segfaults the backend, in a release build with no
		 * assertion to say why.
		 *
		 * There is nothing to reset in any case: the error context belonged
		 * to the task and went with it.  Cloudberry's two catches around
		 * gpos_exec, in COptTasks::Execute and CGPOptimizer, do not reset
		 * either.
		 */
		rc = 0;					// reported through r->raised, not as failure
	}
	GPOS_CATCH_END;

	if (error_buffer != nullptr)
	{
		if (r->raised)
		{
			r->message = CopyOutWide(r, (const wchar_t *) error_buffer);
			TrimLogLine(r->message);
		}
		pfree(error_buffer);
	}

	return rc;
}
}  // namespace

//---------------------------------------------------------------------------
//	Is this operator NDV-preserving?
//
//	The interesting part is not the answer but the eleven OIDs the switch is
//	written over: PostgreSQL names none of them, so the port names them in
//	compat/cb_operator_oids.h.  A wrong OID there would compile and would
//	tell ORCA that a lossy operator preserves distinct values.
//---------------------------------------------------------------------------
static void *
ProbeNDVPreserving(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->flag = gpdb::IsOpNDVPreserving(r->arg);
	return nullptr;
}

extern "C" bool
GpOrcaOpNDVPreserving(Oid opno, bool *raised)
{
	ProbeResult r;

	r.arg = opno;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeNDVPreserving, &r);
	*raised = r.raised;
	return r.flag;
}

//---------------------------------------------------------------------------
//	The access method a relation is stored with, and whether an index access
//	method handler resolves.
//
//	Cloudberry calls GetAmName() and GetIndexAmRoutine(); PostgreSQL 19 spells
//	the first get_am_name() and returns the second as const.
//---------------------------------------------------------------------------
static void *
ProbeRelAmName(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->text = CopyOut(r, gpdb::GetRelAmName(r->arg));
	return nullptr;
}

extern "C" char *
GpOrcaRelAmName(Oid reloid, bool *raised)
{
	ProbeResult r;

	r.arg = reloid;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeRelAmName, &r);
	*raised = r.raised;
	return r.text;
}

static void *
ProbeIndexAmRoutine(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->flag = gpdb::GetIndexAmRoutineFromAmHandler(r->arg) != nullptr;
	return nullptr;
}

extern "C" bool
GpOrcaIndexAmRoutineExists(Oid am_handler, bool *raised)
{
	ProbeResult r;

	r.arg = am_handler;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeIndexAmRoutine, &r);
	*raised = r.raised;
	return r.flag;
}

//---------------------------------------------------------------------------
//	Does this extended statistics object have functional dependencies?
//
//	1 when it has, 0 when it has none, and `raised` when asking was an error.
//
//	This is the one that would have been wrong.  Cloudberry passes
//	allow_null=true to a three-argument statext_dependencies_load();
//	PostgreSQL's takes two and raises when the dependencies are not built.
//	ORCA asks this of every statistics object it meets, so "not built" is the
//	common case, and a port that dropped the third argument would turn every
//	ordinary statistics object into a failed plan.
//---------------------------------------------------------------------------
static void *
ProbeMVDependencies(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->number = gpdb::GetMVDependencies(r->arg) != nullptr ? 1 : 0;
	return nullptr;
}

extern "C" int
GpOrcaMVDependencyState(Oid stat_oid, bool *raised)
{
	ProbeResult r;

	r.arg = stat_oid;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeMVDependencies, &r);
	*raised = r.raised;
	return r.number;
}

//---------------------------------------------------------------------------
//	Has the catalog changed since this backend last asked?
//
//	Registers the invalidation callbacks on first call.  PostgreSQL 19 types
//	the callback's cache id as SysCacheIdentifier rather than int, which is
//	the same argument in C and a different one in C++, so this is where a
//	port that only fixed the compile error would show it.
//---------------------------------------------------------------------------
static void *
ProbeMDCacheNeedsReset(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->flag = gpdb::MDCacheNeedsReset();
	return nullptr;
}

extern "C" bool
GpOrcaMDCacheNeedsReset(bool *raised)
{
	ProbeResult r;

	r.caller = CurrentMemoryContext;
	RunProbe(ProbeMDCacheNeedsReset, &r);
	*raised = r.raised;
	return r.flag;
}

//---------------------------------------------------------------------------
//	A wrapper that belongs to a later milestone.
//
//	Returns the ORCA exception it raised, as "major/minor", so that a test can
//	see the fallback path works rather than trusting that it does.  Which
//	wrapper is chosen does not matter much; this one is reached for every
//	distributed table, so it is the one M2 will fill in first.
//---------------------------------------------------------------------------
static void *
ProbeUnported(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->number = gpdb::CdbHashRandomSeg(2);
	return nullptr;
}

extern "C" char *
GpOrcaUnportedRaise(void)
{
	ProbeResult r;
	char		buf[64];

	r.caller = CurrentMemoryContext;
	RunProbe(ProbeUnported, &r);

	if (!r.raised)
	{
		return nullptr;
	}

	snprintf(buf, sizeof(buf), "%u/%u", (unsigned) r.major, (unsigned) r.minor);
	return MemoryContextStrdup(r.caller, buf);
}

//---------------------------------------------------------------------------
//	What distribution policy does ORCA see for this relation?
//
//	"entry", "replicated", "partitioned" or "random", or nothing when the
//	relation has no policy.  The wrapper is the port's own -- Cloudberry reads
//	gp_distribution_policy and the port reads the "gp" security label -- and
//	it is asked of every relation the relcache translator meets, which is why
//	it had to work at M1 rather than M2.
//
//	A relation with no policy is ordinary here and impossible in Cloudberry,
//	whose DDL gives every table one.
//---------------------------------------------------------------------------
static void *
ProbePolicyKind(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	// The wrapper is held, not .get()'d off a temporary.  A temporary
	// RelationWrapper is destroyed at the end of the full expression, and
	// its destructor closes the relation -- so `Relation rel =
	// GetRelation(oid).get();` hands back a pointer to a relation that has
	// already been closed, and the next line reads freed memory.  That is
	// the mistake RelationWrapper exists to prevent, and it is still
	// available to anyone who writes .get() one line too early.
	gpdb::RelationWrapper rel = gpdb::GetRelation(r->arg);
	GpPolicy   *policy;

	if (!rel)
	{
		return nullptr;
	}

	policy = gpdb::GetDistributionPolicy(rel.get());
	if (policy == nullptr)
	{
		r->text = nullptr;
	}
	else if (GpPolicyIsEntry(policy))
	{
		r->text = CopyOut(r, "entry");
	}
	else if (GpPolicyIsReplicated(policy))
	{
		r->text = CopyOut(r, "replicated");
	}
	else if (GpPolicyIsHashPartitioned(policy))
	{
		r->text = CopyOut(r, "partitioned");
	}
	else if (GpPolicyIsRandomPartitioned(policy))
	{
		r->text = CopyOut(r, "random");
	}
	else
	{
		r->text = CopyOut(r, "unknown");
	}

	// No CloseRelation: the wrapper does it when it goes out of scope.
	return nullptr;
}

extern "C" char *
GpOrcaPolicyKind(Oid relid, bool *raised)
{
	ProbeResult r;

	r.arg = relid;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbePolicyKind, &r);
	*raised = r.raised;
	return r.text;
}

//---------------------------------------------------------------------------
//	Replay the planner's aggregate bookkeeping over the port's copies of
//	find_compatible_agg() and find_compatible_trans().
//
//	Those two decide which aggregates in a query may share an Agg node's
//	transition state.  They are the last two functions of the compat layer
//	with no caller -- CTranslatorDXLToPlStmt is the only one there will be --
//	and a wrong answer from either is a wrong plan rather than a failure: too
//	much sharing computes one aggregate's state and reads it as another's.
//
//	So they are tested against the planner rather than against expectations.
//	PostgreSQL 19 records the answer on the node: Aggref.aggno and
//	Aggref.aggtransno are -1 after parsing and are set by preprocess_aggref
//	(prepagg.c), so gp_orca.planner_aggnos() reads what the planner decided
//	and this produces what the port would have decided, over the same Aggrefs
//	in the same order.  Any difference is a defect.
//
//	The loop is preprocess_aggref's, minus what only PlannerInfo needs:
//	numOrderedAggs and the partial-aggregation flags, which record what a
//	plan may do later and take no part in choosing aggno or transno.
//---------------------------------------------------------------------------
static void *
ProbeReplayAggrefs(void *ptr)
{
	ProbeResult *a = (ProbeResult *) ptr;
	List	   *agginfos = NIL;
	List	   *aggtransinfos = NIL;
	ListCell   *lc;
	int			n = list_length(a->aggrefs);
	int			i = 0;

	a->aggnos = (int *) MemoryContextAlloc(a->caller, sizeof(int) * (n > 0 ? n : 1));
	a->transnos = (int *) MemoryContextAlloc(a->caller, sizeof(int) * (n > 0 ? n : 1));

	foreach(lc, a->aggrefs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);
		Oid			aggtransfn;
		Oid			aggfinalfn;
		Oid			aggcombinefn;
		Oid			aggserialfn;
		Oid			aggdeserialfn;
		Oid			aggtranstype;
		int			aggtransspace;
		Datum		initValue;
		bool		initValueIsNull;
		bool		shareable;
		int32		aggtranstypmod;
		int16		transtypeLen;
		bool		transtypeByVal;
		List	   *same_input_transnos = NIL;
		int			aggno;
		int			transno;

		gpdb::GetAggregateInfo(aggref, &aggtransfn, &aggfinalfn, &aggcombinefn,
							   &aggserialfn, &aggdeserialfn, &aggtranstype,
							   &aggtransspace, &initValue, &initValueIsNull,
							   &shareable);

		// The wrapper resolves the polymorphic transition type and hands it
		// back, and does NOT write it into the Aggref -- but
		// find_compatible_agg compares newagg->aggtranstype against the ones
		// it has already seen.  A caller that left this line out would be
		// comparing whatever the parser put there, which for a polymorphic
		// aggregate is the unresolved pseudo-type, and every such aggregate
		// would look compatible with every other.  Cloudberry's translator
		// sets it here too.
		aggref->aggtranstype = aggtranstype;

		aggtranstypmod = -1;
		if (aggref->args)
		{
			TargetEntry *tle = (TargetEntry *) linitial(aggref->args);

			if (aggtranstype == gpdb::ExprType((Node *) tle->expr))
				aggtranstypmod = gpdb::ExprTypeMod((Node *) tle->expr);
		}

		// 1. the same aggregate call as one already seen?
		aggno = gpdb::FindCompatibleAgg(agginfos, aggref, &same_input_transnos);
		if (aggno != -1)
		{
			AggInfo    *agginfo = (AggInfo *) gpdb::ListNth(agginfos, aggno);

			agginfo->aggrefs = gpdb::LAppend(agginfo->aggrefs, aggref);
			transno = agginfo->transno;
		}
		else
		{
			AggInfo    *agginfo = MakeNode(AggInfo);

			agginfo->finalfn_oid = aggfinalfn;
			agginfo->aggrefs = ListMake1(aggref);
			agginfo->shareable = shareable;

			aggno = (int) gpdb::ListLength(agginfos);
			agginfos = gpdb::LAppend(agginfos, agginfo);

			gpdb::TypLenByVal(aggtranstype, &transtypeLen, &transtypeByVal);

			// 2. can it share a transition state with one already set up?
			transno = gpdb::FindCompatibleTrans(aggtransinfos, shareable,
												aggtransfn, aggtranstype,
												transtypeLen, transtypeByVal,
												aggcombinefn,
												aggserialfn, aggdeserialfn,
												initValue, initValueIsNull,
												same_input_transnos);
			if (transno == -1)
			{
				AggTransInfo *transinfo = MakeNode(AggTransInfo);

				transinfo->args = aggref->args;
				transinfo->aggfilter = aggref->aggfilter;
				transinfo->transfn_oid = aggtransfn;
				transinfo->combinefn_oid = aggcombinefn;
				transinfo->serialfn_oid = aggserialfn;
				transinfo->deserialfn_oid = aggdeserialfn;
				transinfo->aggtranstype = aggtranstype;
				transinfo->aggtranstypmod = aggtranstypmod;
				transinfo->transtypeLen = transtypeLen;
				transinfo->transtypeByVal = transtypeByVal;
				transinfo->aggtransspace = aggtransspace;
				transinfo->initValue = initValue;
				transinfo->initValueIsNull = initValueIsNull;

				transno = (int) gpdb::ListLength(aggtransinfos);
				aggtransinfos = gpdb::LAppend(aggtransinfos, transinfo);
			}
			agginfo->transno = transno;
		}

		a->aggnos[i] = aggno;
		a->transnos[i] = transno;
		i++;
	}

	return nullptr;
}

extern "C" int
GpOrcaReplayAggrefs(struct List *aggrefs, int **aggnos, int **transnos,
					bool *raised)
{
	ProbeResult r;

	r.aggrefs = aggrefs;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeReplayAggrefs, &r);

	*raised = r.raised;
	*aggnos = r.aggnos;
	*transnos = r.transnos;
	return list_length(aggrefs);
}

//---------------------------------------------------------------------------
//	What ORCA's metadata accessor says about one catalog object, as DXL.
//
//	The relcache translator's test surface, and its only one until Query to
//	DXL exists to consume it.  It asks for an object the way the optimizer
//	will -- through a CMDAccessor over a CMDProviderRelcache, with the
//	metadata cache in front -- and serializes what comes back.  Each kind is
//	one way into CTranslatorRelcacheToDXL:
//
//	  relation          RetrieveRel: columns, keys, indexes, distribution
//	  index             RetrieveIndex
//	  check_constraint  RetrieveCheckConstraints, which is the scalar
//	                    translator's stand-alone path
//	  type, operator, function, aggregate
//	  relation_stats    RetrieveRelStats
//	  column_stats      RetrieveColStats, which turns a column's MCVs and
//	                    histogram bounds into ORCA datums through the scalar
//	                    translator
//
//	The metadata cache is handled as COptTasks::OptimizeTask handles it:
//	built on first use, reset when the catalog has changed since the last
//	time, and shut down if a lookup fails, so that nothing half-read stays
//	cached.  The kind has been checked by the caller.
//---------------------------------------------------------------------------
// The metadata probe speaks ORCA's metadata and DXL types, which live in three
// namespaces besides gpos; the translator's own files open all four the same
// way.
using namespace gpdxl;
using namespace gpmd;
using namespace gpopt;

namespace
{
const CSystemId probe_sysid(IMDId::EmdidGeneral, GPOS_WSZ_STR_LENGTH("GPDB"));

void
EnsureMDCache(void)
{
	// Called every time, as COptTasks calls it: the first call is what
	// registers the invalidation callbacks the answer depends on.
	bool		reset = gpdb::MDCacheNeedsReset();
	ULLONG		quota = (ULLONG) optimizer_mdcache_size * 1024L;

	if (!CMDCache::FInitialized())
	{
		CMDCache::Init();
		CMDCache::SetCacheQuota(quota);
	}
	else if (reset)
	{
		CMDCache::Reset();
		CMDCache::SetCacheQuota(quota);
	}
	else if (CMDCache::ULLGetCacheQuota() != quota)
	{
		CMDCache::SetCacheQuota(quota);
	}
}

const IMDCacheObject *
RetrieveByKind(CMemoryPool *mp, CMDAccessor *mda, const char *kind, Oid oid,
			   int attno, IMDId **mdid_out)
{
	IMDId	   *mdid = nullptr;
	const IMDCacheObject *obj = nullptr;

	if (0 == strcmp(kind, "relation"))
	{
		mdid = GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidRel, oid);
		obj = mda->RetrieveRel(mdid);
	}
	else if (0 == strcmp(kind, "index"))
	{
		mdid = GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidInd, oid);
		obj = mda->RetrieveIndex(mdid);
	}
	else if (0 == strcmp(kind, "check_constraint"))
	{
		mdid = GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidCheckConstraint, oid);
		obj = mda->RetrieveCheckConstraints(mdid);
	}
	else if (0 == strcmp(kind, "type"))
	{
		mdid = GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidGeneral, oid);
		obj = mda->RetrieveType(mdid);
	}
	else if (0 == strcmp(kind, "operator"))
	{
		mdid = GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidGeneral, oid);
		obj = mda->RetrieveScOp(mdid);
	}
	else if (0 == strcmp(kind, "function"))
	{
		mdid = GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidGeneral, oid);
		obj = mda->RetrieveFunc(mdid);
	}
	else if (0 == strcmp(kind, "aggregate"))
	{
		mdid = GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidGeneral, oid);
		obj = mda->RetrieveAgg(mdid);
	}
	else if (0 == strcmp(kind, "relation_stats"))
	{
		// The stats mdid takes over the relation's.
		mdid = GPOS_NEW(mp)
			CMDIdRelStats(GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidRel, oid));
		obj = mda->Pmdrelstats(mdid);
	}
	else
	{
		GPOS_ASSERT(0 == strcmp(kind, "column_stats"));
		// A position in ORCA's column list rather than an attribute number:
		// the list holds every attribute, dropped ones included, in attnum
		// order, so the position is attnum - 1.
		mdid = GPOS_NEW(mp) CMDIdColStats(
			GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidRel, oid), (ULONG) (attno - 1));
		obj = mda->Pmdcolstats(mdid);
	}

	*mdid_out = mdid;
	return obj;
}

void *
ProbeMDDxl(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;
	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	EnsureMDCache();

	CMDProviderRelcache *provider = GPOS_NEW(mp) CMDProviderRelcache();

	GPOS_TRY
	{
		// The accessor takes over the reference GPOS_NEW gave the provider --
		// RegisterProvider stores it without an AddRef, and the accessor's
		// provider element releases it on the way out -- so nothing here
		// releases it, as nothing in COptTasks does.  (RegisterProviders, the
		// array form, is the one that adds a reference; reading its AddRef as
		// RegisterProvider's is what the first version of this probe did, and
		// it freed the provider twice.)
		CMDAccessor mda(mp, CMDCache::Pcache(), probe_sysid, provider);

		// An optimizer context, as COptimizer sets one up around every
		// optimization.  ORCA's metadata code assumes it runs inside one:
		// turning a column's MCVs and histogram bounds into ORCA datums asks
		// the context for the metadata accessor, and without one the first
		// column_stats request dereferenced a null context and took the
		// backend down.  The other kinds happen not to need it, which is why
		// they worked first.  Default configuration, and the constant
		// evaluator that evaluates nothing: this plans nothing.
		// The null configuration is spelled with its type: CAutoOptCtxt has
		// a second constructor that takes an ICostModel in the same place.
		CAutoOptCtxt aoc(mp, &mda, nullptr /* constant evaluator */,
						 static_cast<COptimizerConfig *>(nullptr));

		IMDId	   *mdid = nullptr;
		const IMDCacheObject *obj =
			RetrieveByKind(mp, &mda, r->kind, r->arg, r->attno, &mdid);

		// Inside the accessor's scope: the object is pinned only while the
		// accessor holds it.
		CWStringDynamic *dxl = CDXLUtils::SerializeMDObj(
			mp, obj, false /* document header and footer */,
			true /* indentation */);

		r->text = CopyOutWide(r, dxl->GetBuffer());
		GPOS_DELETE(dxl);
		mdid->Release();
	}
	GPOS_CATCH_EX(ex)
	{
		CMDCache::Shutdown();
		GPOS_RETHROW(ex);
	}
	GPOS_CATCH_END;

	if (!optimizer_metadata_caching)
	{
		CMDCache::Shutdown();
	}

	return nullptr;
}
}  // namespace

extern "C" char *
GpOrcaMDDxl(const char *kind, Oid oid, int attno, bool *raised,
			bool *from_postgres, char **message)
{
	ProbeResult r;

	r.kind = kind;
	r.arg = oid;
	r.attno = attno;
	r.caller = CurrentMemoryContext;
	r.want_message = true;
	RunProbe(ProbeMDDxl, &r);
	*raised = r.raised;
	*from_postgres = r.from_postgres;
	*message = r.message;
	return r.text;
}
