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
//		RelationWrapper.h
//
//	@doc:
//		An RAII handle for a PostgreSQL Relation.
//
//		Carried from github/cloudberry/src/include/gpopt/utils/RelationWrapper.h
//		unchanged but for this block.  It is the one file of the translator
//		that needed no porting at all: it names `Relation` and nothing else of
//		PostgreSQL's, and it names it through a forward declaration.
//
//		It lives here rather than being included from Cloudberry's tree
//		because the translator is the port's own code and a merge from
//		apache/cloudberry must not silently change it.  If it changes
//		upstream, it is ported by hand like the rest.
//
//---------------------------------------------------------------------------
#ifndef GPDB_RelationWrapper_H
#define GPDB_RelationWrapper_H

#include <cstddef>

using Relation = struct RelationData *;

namespace gpdb
{
/// \class
/// A transparent RAII wrapper for a pointer to a Postgres RelationData.
/// "Transparent" means that an object of this type can be used in most contexts
/// that expect a Relation. The main advantage of using this wrapper is that it
/// automatically closes the wrapper relation when exiting scope. So you no
/// longer have to write code like this:
/// \code
/// void RetrieveRel(Oid reloid) {
///     Relation rel = GetRelation(reloid);
///     if (IsPartialDist(rel)) {
///         CloseRelation(rel);
///         GPOS_RAISE(...);
///     }
///     try {
///         do_stuff();
///         CloseRelation(rel);
///     catch (...) {
///         CloseRelation(rel);
///         GPOS_RETHROW(...);
///     }
/// }
/// \endcode
/// and instead you can write this:
/// \code
/// void RetrieveRel(Oid reloid) {
///     gpdb::RelationWrapper rel = GetRelation(reloid);
///     if (IsPartialDist(rel.get())) {
///         GPOS_RAISE(...);
///     }
///     do_stuff();
/// }
/// \endcode
class RelationWrapper
{
public:
	RelationWrapper(RelationWrapper const &) = delete;
	RelationWrapper(RelationWrapper &&r) : m_relation(r.m_relation)
	{
		r.m_relation = nullptr;
	};

	explicit RelationWrapper(Relation relation) : m_relation(relation)
	{
	}

	/// allows use in typical conditionals of the form
	///
	/// \code if (rel) { do_stuff(rel); } \endcode or
	/// \code if (!rel) return; \endcode
	explicit operator bool() const
	{
		return m_relation != nullptr;
	}

	// behave like a raw pointer on arrow
	Relation
	operator->() const
	{
		return m_relation;
	}

	// get the raw pointer, behaves like std::unique_ptr::get()
	Relation
	get() const
	{
		return m_relation;
	}

	/// Explicitly close the underlying relation early. This is not usually
	/// necessary unless there is significant amount of time between the point
	/// of close and the end-of-scope
	void Close();

	~RelationWrapper() noexcept(false);

private:
	Relation m_relation = nullptr;
};
}  // namespace gpdb
#endif	// GPDB_RelationWrapper_H
