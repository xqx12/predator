/*
 * Copyright (C) 2011 Jiri Simacek
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CALL_H
#define CALL_H

#include <vector>
#include <unordered_map>

#include "abstractinstruction.hh"
/*
class FI_call : public AbstractInstruction {

	const CodeStorage::Fnc& fnc_;

	AbstractInstruction* target_;

public:

	FI_call(const CodeStorage::Fnc& fnc) : AbstractInstruction(), fnc_(fnc), target_(NULL) {}

	virtual void execute(ExecutionManager& execMan, const AbstractInstruction::StateType& state);
	
	virtual void finalize(
		const std::unordered_map<const CodeStorage::Block*, AbstractInstruction*>&,
		std::vector<AbstractInstruction*>::const_iterator
	);

	virtual std::ostream& toStream(std::ostream& os) const {
		return os << "call  \t" << this->target_;
	}

};
*/
class FI_ret : public AbstractInstruction {

	size_t dst_;

public:

	FI_ret(size_t dst) : AbstractInstruction(), dst_(dst) {}

	virtual void execute(ExecutionManager& execMan, const AbstractInstruction::StateType& state);
	
	virtual void finalize(
		const std::unordered_map<const CodeStorage::Block*, AbstractInstruction*>&,
		std::vector<AbstractInstruction*>::const_iterator
	);

	virtual std::ostream& toStream(std::ostream& os) const {
		return os << "ret   \tr" << this->dst_;
	}

};

#endif