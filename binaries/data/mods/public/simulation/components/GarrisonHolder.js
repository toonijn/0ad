function GarrisonHolder() {}

GarrisonHolder.prototype.Schema =
	"<element name='Max' a:help='Maximum number of entities which can be garrisoned in this holder'>" +
		"<data type='positiveInteger'/>" +
	"</element>" +
	"<element name='List' a:help='Classes of entities which are allowed to garrison in this holder (from Identity)'>" +
		"<attribute name='datatype'>" +
			"<value>tokens</value>" +
		"</attribute>" +
		"<text/>" +
	"</element>" +
	"<element name='EjectClassesOnDestroy' a:help='Classes of entities to be ejected on destroy. Others are killed'>" +
		"<attribute name='datatype'>" +
			"<value>tokens</value>" +
		"</attribute>" +
		"<text/>" +
	"</element>" +
	"<element name='BuffHeal' a:help='Number of hitpoints that will be restored to this holder&apos;s garrisoned units each second'>" +
		"<ref name='nonNegativeDecimal'/>" +
	"</element>" +
	"<element name='LoadingRange' a:help='The maximum distance from this holder at which entities are allowed to garrison. Should be about 2.0 for land entities and preferably greater for ships'>" +
		"<ref name='nonNegativeDecimal'/>" +
	"</element>" +
	"<optional>" +
		"<element name='EjectHealth' a:help='Percentage of maximum health below which this holder no longer allows garrisoning'>" +
			"<ref name='nonNegativeDecimal'/>" +
		"</element>" +
	"</optional>" +
	"<optional>" +
		"<element name='Pickup' a:help='This garrisonHolder will move to pick up units to be garrisoned'>" +
			"<data type='boolean'/>" +
		"</element>" +
	"</optional>";

/**
 * Initialize GarrisonHolder Component
 * Garrisoning when loading a map is set in the script of the map, by setting initGarrison
 * which should contain the array of garrisoned entities.
 */
GarrisonHolder.prototype.Init = function()
{
	// Garrisoned Units
	this.entities = [];
	this.timer = undefined;
	this.allowGarrisoning = new Map();
};

/**
 * @param {number} entity - The entity to verify.
 * @return {boolean} - Whether the given entity is garrisoned in this GarrisonHolder.
 */
GarrisonHolder.prototype.IsGarrisoned = function(entity)
{
	return this.entities.indexOf(entity) != -1;
};

/**
 * @return {Object} max and min range at which entities can garrison the holder.
 */
GarrisonHolder.prototype.GetLoadingRange = function()
{
	return { "max": +this.template.LoadingRange, "min": 0 };
};

GarrisonHolder.prototype.CanPickup = function(ent)
{
	if (!this.template.Pickup || this.IsFull())
		return false;
	let cmpOwner = Engine.QueryInterface(this.entity, IID_Ownership);
	return !!cmpOwner && IsOwnedByPlayer(cmpOwner.GetOwner(), ent);
};

GarrisonHolder.prototype.GetEntities = function()
{
	return this.entities;
};

/**
 * @return {Array} unit classes which can be garrisoned inside this
 * particular entity. Obtained from the entity's template.
 */
GarrisonHolder.prototype.GetAllowedClasses = function()
{
	return this.template.List._string;
};

GarrisonHolder.prototype.GetCapacity = function()
{
	return ApplyValueModificationsToEntity("GarrisonHolder/Max", +this.template.Max, this.entity);
};

GarrisonHolder.prototype.IsFull = function()
{
	return this.GetGarrisonedEntitiesCount() >= this.GetCapacity();
};

GarrisonHolder.prototype.GetHealRate = function()
{
	return ApplyValueModificationsToEntity("GarrisonHolder/BuffHeal", +this.template.BuffHeal, this.entity);
};

/**
 * Set this entity to allow or disallow garrisoning in the entity.
 * Every component calling this function should do it with its own ID, and as long as one
 * component doesn't allow this entity to garrison, it can't be garrisoned
 * When this entity already contains garrisoned soldiers,
 * these will not be able to ungarrison until the flag is set to true again.
 *
 * This more useful for modern-day features. For example you can't garrison or ungarrison
 * a driving vehicle or plane.
 * @param {boolean} allow - Whether the entity should be garrisonable.
 */
GarrisonHolder.prototype.AllowGarrisoning = function(allow, callerID)
{
	this.allowGarrisoning.set(callerID, allow);
};

GarrisonHolder.prototype.IsGarrisoningAllowed = function()
{
	return Array.from(this.allowGarrisoning.values()).every(allow => allow);
};

GarrisonHolder.prototype.GetGarrisonedEntitiesCount = function()
{
	let count = this.entities.length;
	for (let ent of this.entities)
	{
		let cmpGarrisonHolder = Engine.QueryInterface(ent, IID_GarrisonHolder);
		if (cmpGarrisonHolder)
			count += cmpGarrisonHolder.GetGarrisonedEntitiesCount();
	}
	return count;
};

GarrisonHolder.prototype.IsAllowedToGarrison = function(entity)
{
	if (!this.IsGarrisoningAllowed())
		return false;

	if (!IsOwnedByMutualAllyOfEntity(entity, this.entity))
		return false;

	let extraCount = 0;
	let cmpGarrisonHolder = Engine.QueryInterface(entity, IID_GarrisonHolder);
	if (cmpGarrisonHolder)
		extraCount += cmpGarrisonHolder.GetGarrisonedEntitiesCount();
	if (this.GetGarrisonedEntitiesCount() + extraCount >= this.GetCapacity())
		return false;

	let cmpIdentity = Engine.QueryInterface(entity, IID_Identity);
	if (!cmpIdentity)
		return false;

	let entityClasses = cmpIdentity.GetClassesList();
	return MatchesClassList(entityClasses, this.template.List._string) && !!Engine.QueryInterface(entity, IID_Garrisonable);
};

/**
 * @param {number} entity - The entityID to garrison.
 * @param {boolean} renamed - Whether the entity was renamed.
 *
 * @return {boolean} - Whether the entity was garrisoned.
 */
GarrisonHolder.prototype.Garrison = function(entity, renamed = false)
{
	if (!this.IsAllowedToGarrison(entity))
		return false;

	if (!this.HasEnoughHealth())
		return false;

	if (!this.timer && this.GetHealRate() > 0)
	{
		let cmpTimer = Engine.QueryInterface(SYSTEM_ENTITY, IID_Timer);
		this.timer = cmpTimer.SetTimeout(this.entity, IID_GarrisonHolder, "HealTimeout", 1000, {});
	}

	this.entities.push(entity);
	this.UpdateGarrisonFlag();
	let cmpProductionQueue = Engine.QueryInterface(entity, IID_ProductionQueue);
	if (cmpProductionQueue)
		cmpProductionQueue.PauseProduction();

	let cmpAura = Engine.QueryInterface(entity, IID_Auras);
	if (cmpAura && cmpAura.HasGarrisonAura())
		cmpAura.ApplyGarrisonAura(this.entity);

	let cmpPosition = Engine.QueryInterface(entity, IID_Position);
	if (cmpPosition)
		cmpPosition.MoveOutOfWorld();

	Engine.PostMessage(this.entity, MT_GarrisonedUnitsChanged, {
		"added": [entity],
		"removed": [],
		"renamed": renamed
	});

	return true;
};

/**
 * Simply eject the unit from the garrisoning entity without moving it
 * @param {number} entity - Id of the entity to be ejected.
 * @param {boolean} forced - Whether eject is forced (i.e. if building is destroyed).
 * @param {boolean} renamed - Whether eject was due to entity renaming.
 *
 * @return {boolean} Whether the entity was ejected.
 */
GarrisonHolder.prototype.Eject = function(entity, forced, renamed = false)
{
	let entityIndex = this.entities.indexOf(entity);
	// Error: invalid entity ID, usually it's already been ejected
	if (entityIndex == -1)
		return false;

	// Find spawning location
	let cmpFootprint = Engine.QueryInterface(this.entity, IID_Footprint);
	let cmpHealth = Engine.QueryInterface(this.entity, IID_Health);
	let cmpIdentity = Engine.QueryInterface(this.entity, IID_Identity);

	// If the garrisonHolder is a sinking ship, restrict the location to the intersection of both passabilities
	// TODO: should use passability classes to be more generic
	let pos;
	if ((!cmpHealth || cmpHealth.GetHitpoints() == 0) && cmpIdentity && cmpIdentity.HasClass("Ship"))
		pos = cmpFootprint.PickSpawnPointBothPass(entity);
	else
		pos = cmpFootprint.PickSpawnPoint(entity);

	if (pos.y < 0)
	{
		// Error: couldn't find space satisfying the unit's passability criteria
		if (!forced)
			return false;

		// If ejection is forced, we need to continue, so use center of the building
		let cmpPosition = Engine.QueryInterface(this.entity, IID_Position);
		pos = cmpPosition.GetPosition();
	}

	this.entities.splice(entityIndex, 1);

	// Reset the obstruction flags to template defaults.
	let cmpObstruction = Engine.QueryInterface(entity, IID_Obstruction);
	if (cmpObstruction)
		cmpObstruction.SetActive(true);

	let cmpEntUnitAI = Engine.QueryInterface(entity, IID_UnitAI);
	if (cmpEntUnitAI)
		cmpEntUnitAI.Ungarrison();

	let cmpEntProductionQueue = Engine.QueryInterface(entity, IID_ProductionQueue);
	if (cmpEntProductionQueue)
		cmpEntProductionQueue.UnpauseProduction();

	let cmpEntAura = Engine.QueryInterface(entity, IID_Auras);
	if (cmpEntAura && cmpEntAura.HasGarrisonAura())
		cmpEntAura.RemoveGarrisonAura(this.entity);

	let cmpEntPosition = Engine.QueryInterface(entity, IID_Position);
	if (cmpEntPosition)
	{
		cmpEntPosition.JumpTo(pos.x, pos.z);
		cmpEntPosition.SetHeightOffset(0);

		let cmpPosition = Engine.QueryInterface(this.entity, IID_Position);
		if (cmpPosition)
			cmpEntPosition.SetYRotation(cmpPosition.GetPosition().horizAngleTo(pos));
	}

	Engine.PostMessage(this.entity, MT_GarrisonedUnitsChanged, {
		"added": [],
		"removed": [entity],
		"renamed": renamed
	});

	return true;
};

/**
 * Ejects units and orders them to move to the rally point. If an ejection
 * with a given obstruction radius has failed, we won't try anymore to eject
 * entities with a bigger obstruction as that is compelled to also fail.
 * @param {Array} entities - An array containing the ids of the entities to eject.
 * @param {boolean} forced - Whether eject is forced (ie: if building is destroyed).
 * @return {boolean} Whether the entities were ejected.
 */
GarrisonHolder.prototype.PerformEject = function(entities, forced)
{
	if (!this.IsGarrisoningAllowed() && !forced)
		return false;

	let ejectedEntities = [];
	let success = true;
	let failedRadius;
	let radius;
	let cmpOwnership = Engine.QueryInterface(this.entity, IID_Ownership);

	for (let entity of entities)
	{
		if (failedRadius !== undefined)
		{
			let cmpObstruction = Engine.QueryInterface(entity, IID_Obstruction);
			radius = cmpObstruction ? cmpObstruction.GetUnitRadius() : 0;
			if (radius >= failedRadius)
				continue;
		}

		if (this.Eject(entity, forced))
		{
			let cmpEntOwnership = Engine.QueryInterface(entity, IID_Ownership);
			if (cmpOwnership && cmpEntOwnership && cmpOwnership.GetOwner() == cmpEntOwnership.GetOwner())
				ejectedEntities.push(entity);
		}
		else
		{
			success = false;
			if (failedRadius !== undefined)
				failedRadius = Math.min(failedRadius, radius);
			else
			{
				let cmpObstruction = Engine.QueryInterface(entity, IID_Obstruction);
				failedRadius = cmpObstruction ? cmpObstruction.GetUnitRadius() : 0;
			}
		}
	}

	this.OrderWalkToRallyPoint(ejectedEntities);
	this.UpdateGarrisonFlag();

	return success;
};

/**
 * Order entities to walk to the rally point.
 * @param {Array} entities - An array containing all the ids of the entities.
 */
GarrisonHolder.prototype.OrderWalkToRallyPoint = function(entities)
{
	let cmpOwnership = Engine.QueryInterface(this.entity, IID_Ownership);
	let cmpRallyPoint = Engine.QueryInterface(this.entity, IID_RallyPoint);
	if (!cmpRallyPoint || !cmpRallyPoint.GetPositions()[0])
		return;

	let commands = GetRallyPointCommands(cmpRallyPoint, entities);
	// Ignore the rally point if it is autogarrison
	if (commands[0].type == "garrison" && commands[0].target == this.entity)
		return;

	for (let command of commands)
		ProcessCommand(cmpOwnership.GetOwner(), command);
};

/**
 * Unload unit from the garrisoning entity and order them
 * to move to the rally point.
 * @return {boolean} Whether the command was successful.
 */
GarrisonHolder.prototype.Unload = function(entity, forced)
{
	return this.PerformEject([entity], forced);
};

/**
 * Unload one or all units that match a template and owner from
 * the garrisoning entity and order them to move to the rally point.
 * @param {string} template - Type of units that should be ejected.
 * @param {number} owner - Id of the player whose units should be ejected.
 * @param {boolean} all - Whether all units should be ejected.
 * @param {boolean} forced - Whether unload is forced.
 * @return {boolean} Whether the unloading was successful.
 */
GarrisonHolder.prototype.UnloadTemplate = function(template, owner, all, forced)
{
	let entities = [];
	let cmpTemplateManager = Engine.QueryInterface(SYSTEM_ENTITY, IID_TemplateManager);
	for (let entity of this.entities)
	{
		let cmpIdentity = Engine.QueryInterface(entity, IID_Identity);

		// Units with multiple ranks are grouped together.
		let name = cmpIdentity.GetSelectionGroupName() || cmpTemplateManager.GetCurrentTemplateName(entity);
		if (name != template || owner != Engine.QueryInterface(entity, IID_Ownership).GetOwner())
			continue;

		entities.push(entity);

		// If 'all' is false, only ungarrison the first matched unit.
		if (!all)
			break;
	}

	return this.PerformEject(entities, forced);
};

/**
 * Unload all units, that belong to certain player
 * and order all own units to move to the rally point.
 * @param {boolean} forced - Whether unload is forced.
 * @param {number} owner - Id of the player whose units should be ejected.
 * @return {boolean} Whether the unloading was successful.
 */
GarrisonHolder.prototype.UnloadAllByOwner = function(owner, forced)
{
	let entities = this.entities.filter(ent => {
		let cmpOwnership = Engine.QueryInterface(ent, IID_Ownership);
		return cmpOwnership && cmpOwnership.GetOwner() == owner;
	});
	return this.PerformEject(entities, forced);
};

/**
 * Unload all units from the entity and order them to move to the rally point.
 * @param {boolean} forced - Whether unload is forced.
 * @return {boolean} Whether the unloading was successful.
 */
GarrisonHolder.prototype.UnloadAll = function(forced)
{
	return this.PerformEject(this.entities.slice(), forced);
};

/**
 * Used to check if the garrisoning entity's health has fallen below
 * a certain limit after which all garrisoned units are unloaded.
 */
GarrisonHolder.prototype.OnHealthChanged = function(msg)
{
	if (!this.HasEnoughHealth() && this.entities.length)
		this.EjectOrKill(this.entities.slice());
};

GarrisonHolder.prototype.HasEnoughHealth = function()
{
	// 0 is a valid value so explicitly check for undefined.
	if (this.template.EjectHealth === undefined)
		return true;

	let cmpHealth = Engine.QueryInterface(this.entity, IID_Health);
	return !cmpHealth || cmpHealth.GetHitpoints() > Math.floor(+this.template.EjectHealth * cmpHealth.GetMaxHitpoints());
};

/**
 * Called every second. Heals garrisoned units.
 */
GarrisonHolder.prototype.HealTimeout = function(data)
{
	let cmpTimer = Engine.QueryInterface(SYSTEM_ENTITY, IID_Timer);
	if (!this.entities.length)
	{
		cmpTimer.CancelTimer(this.timer);
		this.timer = undefined;
		return;
	}

	for (let entity of this.entities)
	{
		let cmpHealth = Engine.QueryInterface(entity, IID_Health);
		if (cmpHealth && !cmpHealth.IsUnhealable())
			cmpHealth.Increase(this.GetHealRate());
	}

	this.timer = cmpTimer.SetTimeout(this.entity, IID_GarrisonHolder, "HealTimeout", 1000, {});
};

/**
 * Updates the garrison flag depending whether something is garrisoned in the entity.
 */
GarrisonHolder.prototype.UpdateGarrisonFlag = function()
{
	let cmpVisual = Engine.QueryInterface(this.entity, IID_Visual);
	if (!cmpVisual)
		return;

	cmpVisual.SetVariant("garrison", this.entities.length ? "garrisoned" : "ungarrisoned");
};

/**
 * Cancel timer when destroyed.
 */
GarrisonHolder.prototype.OnDestroy = function()
{
	if (this.timer)
	{
		let cmpTimer = Engine.QueryInterface(SYSTEM_ENTITY, IID_Timer);
		cmpTimer.CancelTimer(this.timer);
	}
};

/**
 * If a garrisoned entity is captured, or about to be killed (so its owner changes to '-1'),
 * remove it from the building so we only ever contain valid entities.
 */
GarrisonHolder.prototype.OnGlobalOwnershipChanged = function(msg)
{
	// The ownership change may be on the garrisonholder
	if (this.entity == msg.entity)
	{
		let entities = this.entities.filter(ent => msg.to == INVALID_PLAYER || !IsOwnedByMutualAllyOfEntity(this.entity, ent));

		if (entities.length)
			this.EjectOrKill(entities);

		return;
	}

	// or on some of its garrisoned units
	let entityIndex = this.entities.indexOf(msg.entity);
	if (entityIndex != -1)
	{
		// If the entity is dead, remove it directly instead of ejecting the corpse
		let cmpHealth = Engine.QueryInterface(msg.entity, IID_Health);
		if (cmpHealth && cmpHealth.GetHitpoints() == 0)
		{
			this.entities.splice(entityIndex, 1);
			Engine.PostMessage(this.entity, MT_GarrisonedUnitsChanged, {
				"added": [],
				"removed": [msg.entity]
			});
			this.UpdateGarrisonFlag();
		}
		else if (msg.to == INVALID_PLAYER || !IsOwnedByMutualAllyOfEntity(this.entity, msg.entity))
			this.EjectOrKill([msg.entity]);
	}
};

/**
 * Update list of garrisoned entities if one gets renamed (e.g. by promotion).
 */
GarrisonHolder.prototype.OnGlobalEntityRenamed = function(msg)
{
	let entityIndex = this.entities.indexOf(msg.entity);
	if (entityIndex != -1)
	{
		this.Eject(msg.entity, true, true);
		this.Garrison(msg.newentity, true);

		// TurretHolder is not subscribed to GarrisonChanged, so we must inform it explicitly.
		// Otherwise a renaming entity may re-occupy another turret instead of its previous one,
		// since the message does not know what turret point whas used, which is not wanted.
		// Also ensure the TurretHolder receives the message after we process it.
		// If it processes it before us we garrison a turret and subsequently
		// are hidden by GarrisonHolder again.
		// This could be fixed by not requiring a turret to be 'garrisoned'.
		let cmpTurretHolder = Engine.QueryInterface(this.entity, IID_TurretHolder);
		if (cmpTurretHolder)
			cmpTurretHolder.SwapEntities(msg.entity, msg.newentity);
	}

	if (!this.initGarrison)
		return;

	// Update the pre-game garrison because of SkirmishReplacement
	if (msg.entity == this.entity)
	{
		let cmpGarrisonHolder = Engine.QueryInterface(msg.newentity, IID_GarrisonHolder);
		if (cmpGarrisonHolder)
			cmpGarrisonHolder.initGarrison = this.initGarrison;
	}
	else
	{
		entityIndex = this.initGarrison.indexOf(msg.entity);
		if (entityIndex != -1)
			this.initGarrison[entityIndex] = msg.newentity;
	}
};

/**
 * Eject all foreign garrisoned entities which are no more allied.
 */
GarrisonHolder.prototype.OnDiplomacyChanged = function()
{
	this.EjectOrKill(this.entities.filter(ent => !IsOwnedByMutualAllyOfEntity(this.entity, ent)));
};

/**
 * Eject or kill a garrisoned unit which can no more be garrisoned
 * (garrisonholder's health too small or ownership changed).
 */
GarrisonHolder.prototype.EjectOrKill = function(entities)
{
	let cmpPosition = Engine.QueryInterface(this.entity, IID_Position);
	// Eject the units which can be ejected (if not in world, it generally means this holder
	// is inside a holder which kills its entities, so do not eject)
	if (cmpPosition && cmpPosition.IsInWorld())
	{
		let ejectables = entities.filter(ent => this.IsEjectable(ent));
		if (ejectables.length)
			this.PerformEject(ejectables, false);
	}

	// And destroy all remaining entities
	let killedEntities = [];
	for (let entity of entities)
	{
		let entityIndex = this.entities.indexOf(entity);
		if (entityIndex == -1)
			continue;
		let cmpHealth = Engine.QueryInterface(entity, IID_Health);
		if (cmpHealth)
			cmpHealth.Kill();
		this.entities.splice(entityIndex, 1);
		killedEntities.push(entity);
	}

	if (killedEntities.length)
	{
		Engine.PostMessage(this.entity, MT_GarrisonedUnitsChanged, {
			"added": [],
			"removed": killedEntities
		});
		this.UpdateGarrisonFlag();
	}
};

/**
 * Whether an entity is ejectable.
 * @param {number} entity - The entity-ID to be tested.
 * @return {boolean} - Whether the entity is ejectable.
 */
GarrisonHolder.prototype.IsEjectable = function(entity)
{
	if (!this.entities.find(ent => ent == entity))
		return false;

	let ejectableClasses = this.template.EjectClassesOnDestroy._string;
	let entityClasses = Engine.QueryInterface(entity, IID_Identity).GetClassesList();

	return MatchesClassList(entityClasses, ejectableClasses);
};

/**
 * Sets the intitGarrison to the specified entities. Used by the mapreader.
 *
 * @param {number[]} entities - The entity IDs to garrison on init.
 */
GarrisonHolder.prototype.SetInitGarrison = function(entities)
{
	this.initGarrison = clone(entities);
};

/**
 * Initialise the garrisoned units.
 */
GarrisonHolder.prototype.OnGlobalInitGame = function(msg)
{
	if (!this.initGarrison)
		return;

	for (let ent of this.initGarrison)
	{
		let cmpUnitAI = Engine.QueryInterface(ent, IID_UnitAI);
		if (cmpUnitAI && cmpUnitAI.CanGarrison(this.entity) && this.Garrison(ent))
			cmpUnitAI.Autogarrison(this.entity);
	}
	this.initGarrison = undefined;
};

GarrisonHolder.prototype.OnValueModification = function(msg)
{
	if (msg.component != "GarrisonHolder" || msg.valueNames.indexOf("GarrisonHolder/BuffHeal") == -1)
		return;

	if (this.timer && this.GetHealRate() == 0)
	{
		let cmpTimer = Engine.QueryInterface(SYSTEM_ENTITY, IID_Timer);
		cmpTimer.CancelTimer(this.timer);
		this.timer = undefined;
	}
	else if (!this.timer && this.GetHealRate() > 0)
	{
		let cmpTimer = Engine.QueryInterface(SYSTEM_ENTITY, IID_Timer);
		this.timer = cmpTimer.SetTimeout(this.entity, IID_GarrisonHolder, "HealTimeout", 1000, {});
	}
};

Engine.RegisterComponentType(IID_GarrisonHolder, "GarrisonHolder", GarrisonHolder);

