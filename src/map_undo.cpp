/*
 *    Copyright 2012 Thomas Schöps
 *    
 *    This file is part of OpenOrienteering.
 * 
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 * 
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 * 
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "map_undo.h"

#include <algorithm>

#include <QIODevice>

#include "map.h"
#include "object.h"
#include "symbol.h"

// ### MapUndoStep ###

MapUndoStep::MapUndoStep(Map* map, Type type): UndoStep(type)
{
	this->map = map;
	layer = map->getCurrentLayerIndex();
}

void MapUndoStep::save(QIODevice* file)
{
	file->write((char*)&layer, sizeof(int));
	int size = affected_objects.size();
	file->write((char*)&size, sizeof(int));
	for (int i = 0; i < size; ++i)
		file->write((char*)&affected_objects[i], sizeof(int));
}
bool MapUndoStep::load(QIODevice* file, int version)
{
	file->read((char*)&layer, sizeof(int));
	int size;
	file->read((char*)&size, sizeof(int));
	affected_objects.resize(size);
	for (int i = 0; i < size; ++i)
		file->read((char*)&affected_objects[i], sizeof(int));
	return true;
}

void MapUndoStep::getAffectedOutcome(std::vector< Object* >& out) const
{
	out.resize(affected_objects.size());
	for (int i = 0; i < (int)affected_objects.size(); ++i)
		out[i] = map->getLayer(layer)->getObject(affected_objects[i]);
}

// ### ObjectContainingUndoStep ###

ObjectContainingUndoStep::ObjectContainingUndoStep(Map* map, Type type) : MapUndoStep(map, type)
{
	connect(map, SIGNAL(symbolChanged(int,Symbol*,Symbol*)), this, SLOT(symbolChanged(int,Symbol*,Symbol*)));
	connect(map, SIGNAL(symbolDeleted(int,Symbol*)), this, SLOT(symbolDeleted(int,Symbol*)));
}
ObjectContainingUndoStep::~ObjectContainingUndoStep()
{
	int size = (int)objects.size();
	for (int i = 0; i < size; ++i)
		delete objects[i];
}

void ObjectContainingUndoStep::addObject(int existing_index, Object* object)
{
	affected_objects.push_back(existing_index);
	object->setMap(map); // this is necessary so the object will find the symbols and colors it references when the undo step is saved
	objects.push_back(object);
}
void ObjectContainingUndoStep::addObject(Object* existing, Object* object)
{
	int index = map->getCurrentLayer()->findObjectIndex(existing);
	assert(index >= 0);
	addObject(index, object);
}

void ObjectContainingUndoStep::save(QIODevice* file)
{
	MapUndoStep::save(file);
	
	int size = (int)objects.size();
	for (int i = 0; i < size; ++i)
	{
		int save_type = static_cast<int>(objects[i]->getType());
		file->write((const char*)&save_type, sizeof(int));
		
		objects[i]->setMap(map);	// IMPORTANT: only if the object's map pointer is set it will save its symbol index correctly
		objects[i]->save(file);
	}
}
bool ObjectContainingUndoStep::load(QIODevice* file, int version)
{
	if (!MapUndoStep::load(file, version))
		return false;
	
	int size = (int)affected_objects.size();
	objects.resize(size);
	for (int i = 0; i < size; ++i)
	{
		int save_type;
		file->read((char*)&save_type, sizeof(int));
		objects[i] = Object::getObjectForType(static_cast<Object::Type>(save_type), NULL);
		if (!objects[i])
			return false;
		objects[i]->load(file, version, map);
	}
	return true;
}

void ObjectContainingUndoStep::symbolChanged(int pos, Symbol* new_symbol, Symbol* old_symbol)
{
	int size = (int)objects.size();
	for (int i = 0; i < size; ++i)
	{
		if (objects[i]->getSymbol() == old_symbol)
			objects[i]->setSymbol(new_symbol, true);
	}
}
void ObjectContainingUndoStep::symbolDeleted(int pos, Symbol* old_symbol)
{
	int size = (int)objects.size();
	for (int i = 0; i < size; ++i)
	{
		if (objects[i]->getSymbol() == old_symbol)
		{
			valid = false;
			return;
		}
	}
}

// ### ReplaceObjectsUndoStep ###

ReplaceObjectsUndoStep::ReplaceObjectsUndoStep(Map* map) : ObjectContainingUndoStep(map, ReplaceObjectsUndoStepType)
{
}
UndoStep* ReplaceObjectsUndoStep::undo()
{
	ReplaceObjectsUndoStep* undo_step = new ReplaceObjectsUndoStep(map);
	
	MapLayer* layer = map->getLayer(this->layer);
	int size = (int)objects.size();
	for (int i = 0; i < size; ++i)
	{
		undo_step->addObject(affected_objects[i], layer->getObject(affected_objects[i]));
		layer->setObject(objects[i], affected_objects[i], false);
	}
	
	objects.clear();
	return undo_step;
}

// ### DeleteObjectsUndoStep ###

DeleteObjectsUndoStep::DeleteObjectsUndoStep(Map* map): MapUndoStep(map, DeleteObjectsUndoStepType)
{
}

void DeleteObjectsUndoStep::addObject(int index)
{
	affected_objects.push_back(index);
}

UndoStep* DeleteObjectsUndoStep::undo()
{
	AddObjectsUndoStep* undo_step = new AddObjectsUndoStep(map);
	
	// Make sure to delete the objects in the right order so the other objects' indices stay valid
	std::sort(affected_objects.begin(), affected_objects.end(), std::greater<int>());
	
	MapLayer* layer = map->getLayer(this->layer);
	int size = (int)affected_objects.size();
	for (int i = 0; i < size; ++i)
	{
		undo_step->addObject(affected_objects[i], layer->getObject(affected_objects[i]));
		layer->deleteObject(affected_objects[i], true);
	}
	
	return undo_step;
}

// ### AddObjectsUndoStep ###

AddObjectsUndoStep::AddObjectsUndoStep(Map* map) : ObjectContainingUndoStep(map, AddObjectsUndoStepType)
{
}
UndoStep* AddObjectsUndoStep::undo()
{
	DeleteObjectsUndoStep* undo_step = new DeleteObjectsUndoStep(map);
	
	// Make sure to add the objects in the right order so the other objects' indices stay valid
	std::vector< std::pair<int, int> > order;	// index into affected_objects & objects, object index
	order.resize(affected_objects.size());
	for (int i = 0; i < (int)affected_objects.size(); ++i)
		order[i] = std::pair<int, int>(i, affected_objects[i]);
	std::sort(order.begin(), order.end(), sortOrder);
	
	MapLayer* layer = map->getLayer(this->layer);
	int size = (int)objects.size();
	for (int i = 0; i < size; ++i)
	{
		undo_step->addObject(affected_objects[order[i].first]);
		layer->addObject(objects[order[i].first], order[i].second);
	}
	
	objects.clear();
	return undo_step;
}
bool AddObjectsUndoStep::sortOrder(const std::pair< int, int >& a, const std::pair< int, int >& b)
{
	return a.second < b.second;
}

// ### SwitchSymbolUndoStep ###

SwitchSymbolUndoStep::SwitchSymbolUndoStep(Map* map) : MapUndoStep(map, SwitchSymbolUndoStepType)
{
	connect(map, SIGNAL(symbolChanged(int,Symbol*,Symbol*)), this, SLOT(symbolChanged(int,Symbol*,Symbol*)));
	connect(map, SIGNAL(symbolDeleted(int,Symbol*)), this, SLOT(symbolDeleted(int,Symbol*)));
}
void SwitchSymbolUndoStep::addObject(int index, Symbol* target_symbol)
{
	affected_objects.push_back(index);
	target_symbols.push_back(target_symbol);
}
UndoStep* SwitchSymbolUndoStep::undo()
{
	SwitchSymbolUndoStep* undo_step = new SwitchSymbolUndoStep(map);
	
	MapLayer* layer = map->getLayer(this->layer);
	int size = (int)affected_objects.size();
	for (int i = 0; i < size; ++i)
	{
		Object* object = layer->getObject(affected_objects[i]);
		undo_step->addObject(affected_objects[i], object->getSymbol());
		assert(object->setSymbol(target_symbols[i], false));
	}
	
	return undo_step;
}
void SwitchSymbolUndoStep::save(QIODevice* file)
{
    MapUndoStep::save(file);
	
	int size = (int)target_symbols.size();
	for (int i = 0; i < size; ++i)
	{
		int index = map->findSymbolIndex(target_symbols[i]);
		file->write((char*)&index, sizeof(int));
	}
}
bool SwitchSymbolUndoStep::load(QIODevice* file, int version)
{
    if (!MapUndoStep::load(file, version))
		return false;
	
	int size = (int)affected_objects.size();
	target_symbols.resize(size);
	for (int i = 0; i < size; ++i)
	{
		int index;
		file->read((char*)&index, sizeof(int));
		target_symbols[i] = map->getSymbol(index);
	}
	return true;
}

void SwitchSymbolUndoStep::symbolChanged(int pos, Symbol* new_symbol, Symbol* old_symbol)
{
	int size = (int)target_symbols.size();
	for (int i = 0; i < size; ++i)
	{
		if (target_symbols[i] == old_symbol)
			target_symbols[i] = new_symbol;
	}
}
void SwitchSymbolUndoStep::symbolDeleted(int pos, Symbol* old_symbol)
{
	int size = (int)target_symbols.size();
	for (int i = 0; i < size; ++i)
	{
		if (target_symbols[i] == old_symbol)
		{
			valid = false;
			return;
		}
	}
}

// ### SwitchDashesUndoStep ###

SwitchDashesUndoStep::SwitchDashesUndoStep(Map* map) : MapUndoStep(map, SwitchDashesUndoStepType)
{
}
void SwitchDashesUndoStep::addObject(int index)
{
	affected_objects.push_back(index);
}
UndoStep* SwitchDashesUndoStep::undo()
{
	SwitchDashesUndoStep* undo_step = new SwitchDashesUndoStep(map);
	
	MapLayer* layer = map->getLayer(this->layer);
	int size = (int)affected_objects.size();
	for (int i = 0; i < size; ++i)
	{
		PathObject* object = reinterpret_cast<PathObject*>(layer->getObject(affected_objects[i]));
		object->reverse();
		object->update(true);
		
		undo_step->addObject(affected_objects[i]);
	}
	
	return undo_step;
}