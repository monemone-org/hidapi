//hid_device_list_node.c


typedef struct hid_device_node_ 
{
	hid_device *dev;
	struct hid_device_node_ *next;

} hid_device_list_node;


//returns updated list
static hid_device_list_node *add_hid_device_to_list(hid_device *new_device, hid_device_list_node *list)
{
	hid_device_list_node *new_node = (hid_device_list_node *)calloc(1, sizeof(hid_device_list_node));
    if (new_node == NULL) {
        return list;
    }
    
	new_node->dev = new_device;

	if (list == NULL)
	{
        list = new_node;
	}
	else
	{
		// find the last non-NULL node
		hid_device_list_node *last = list;
		while (last->next != NULL)
		{			
			last = last->next;
		}

		last->next = new_node;
	}

	return list;
}

//returns updated list
static hid_device_list_node *remove_hid_device_from_list(hid_device *device, hid_device_list_node *list)
{
	// find node that holds the given device
	hid_device_list_node *prev = NULL;
	hid_device_list_node *curr = list;
	while (curr != NULL)
	{	
		if (curr->dev == device)
		{
			break;
		}

		prev = curr;		
		curr = curr->next;
	}

	if (curr == NULL)
	{
		//device not found in hid_device_list
		return list;
	}

	if (prev == NULL) //curr is the 1st node
	{
		assert(curr == list);
		list = curr->next;
	}
	else
	{
		prev->next = curr->next;
	}
	free(curr);
	
	return list;
}



